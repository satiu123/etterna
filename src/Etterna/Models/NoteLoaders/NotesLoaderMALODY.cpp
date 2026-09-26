#include "Etterna/Globals/global.h"
#include "NotesLoaderMALODY.h"
#include "Etterna/Actor/Base/ActorUtil.h"
#include "Etterna/Models/Misc/Difficulty.h"
#include "Etterna/Models/NoteData/NoteData.h"
#include "Etterna/Models/Songs/Song.h"
#include "Etterna/Models/StepsAndStyles/Steps.h"
#include "Etterna/Singletons/GameManager.h"
#include "Etterna/Singletons/PrefsManager.h"
#include "RageUtil/File/RageFile.h"
#include "RageUtil/File/RageFileManager.h"
#include "RageUtil/Utils/RageUtil.h"
#include "RageUtil/Utils/RageUtil_CharConversions.h"

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace MalodyLoader {

static inline auto
ParseBeat(const rapidjson::Value& v) -> double
{
	if (!v.IsArray() || v.Empty()) {
		return 0.0;
	}
	double b0 = v[0].IsNumber() ? v[0].GetDouble() : 0.0;
	if (v.Size() >= 3) {
		double den = v[2].IsNumber() ? v[2].GetDouble() : 0.0;
		double num = v[1].IsNumber() ? v[1].GetDouble() : 0.0;
		if (den != 0.0) {
			return b0 + (num / den);
		}
	} else if (v.Size() >= 2) {
		double num = v[1].IsNumber() ? v[1].GetDouble() : 0.0;
		return b0 + num;
	}
	return b0;
}

static auto
GetStepsType(int columns) -> StepsType
{
	switch (columns) {
		case 3:
			return StepsType_dance_threepanel;
		case 4:
			return StepsType_dance_single;
		case 5:
			return StepsType_pump_single;
		case 6:
			return StepsType_dance_solo;
		case 7:
			return StepsType_kb7_single;
		case 8:
			return StepsType_dance_double;
		case 9:
			return StepsType_popn_nine;
		case 10:
			return StepsType_pump_double;
		default:
			return StepsType_Invalid;
	}
}

static auto
FindSoundFileAndOffset(const rapidjson::Document& doc,
					   const std::string& songDir,
					   std::string& musicFileOut,
					   double& soundBeatOut,
					   float& soundOffsetMsOut) -> bool
{
	musicFileOut.clear();
	soundBeatOut = 0.0;
	soundOffsetMsOut = 0.0f;

	if (doc.HasMember("note") && doc["note"].IsArray()) {
		const auto& notes = doc["note"];
		// Priority 1: type == 1 with sound
		for (const auto& n : notes.GetArray()) {
			if (n.IsObject() && n.HasMember("sound") && n["sound"].IsString()) {
				if (n.HasMember("type") && n["type"].IsInt() &&
					n["type"].GetInt() == 1) {
					musicFileOut = n["sound"].GetString();
					if (n.HasMember("beat")) {
						soundBeatOut = ParseBeat(n["beat"]);
					}
					if (n.HasMember("offset") && n["offset"].IsNumber()) {
						soundOffsetMsOut =
						  static_cast<float>(n["offset"].GetDouble());
					}
					return true;
				}
			}
		}
		// Priority 2: audio file extension
		for (const auto& n : notes.GetArray()) {
			if (n.IsObject() && n.HasMember("sound") && n["sound"].IsString()) {
				std::string s = n["sound"].GetString();
				std::string ext = make_lower(GetExtension(s));
				if (ext == "ogg" || ext == "mp3" || ext == "wav" ||
					ext == "flac" || ext == "oga") {
					musicFileOut = s;
					if (n.HasMember("beat")) {
						soundBeatOut = ParseBeat(n["beat"]);
					}
					if (n.HasMember("offset") && n["offset"].IsNumber()) {
						soundOffsetMsOut =
						  static_cast<float>(n["offset"].GetDouble());
					}
					return true;
				}
			}
		}
		// Priority 3: any sound note
		for (const auto& n : notes.GetArray()) {
			if (n.IsObject() && n.HasMember("sound") && n["sound"].IsString()) {
				musicFileOut = n["sound"].GetString();
				if (n.HasMember("beat")) {
					soundBeatOut = ParseBeat(n["beat"]);
				}
				if (n.HasMember("offset") && n["offset"].IsNumber()) {
					soundOffsetMsOut =
					  static_cast<float>(n["offset"].GetDouble());
				}
				return true;
			}
		}
	}

	// Fallback: search directory for audio files
	const auto& audio_exts = ActorUtil::GetTypeExtensionList(FT_Sound);
	for (const auto& ext : audio_exts) {
		std::vector<std::string> matches;
		FILEMAN->GetDirListing(songDir + "*." + ext, matches, ONLY_FILE);
		if (!matches.empty()) {
			musicFileOut = matches[0];
			return true;
		}
	}

	return false;
}

static void
SetTimingData(const rapidjson::Document& doc,
			  TimingData& timing,
			  double soundBeat,
			  float soundOffsetMs)
{
	struct BpmEntry
	{
		double beat;
		float bpm;
	};
	std::vector<BpmEntry> bpms;

	if (doc.HasMember("time") && doc["time"].IsArray()) {
		for (const auto& t : doc["time"].GetArray()) {
			if (!t.IsObject() || !t.HasMember("beat") || !t.HasMember("bpm")) {
				continue;
			}
			if (!t["bpm"].IsNumber()) {
				continue;
			}
			double b = ParseBeat(t["beat"]);
			float bpm = static_cast<float>(t["bpm"].GetDouble());
			if (bpm > 0.0f) {
				bpms.push_back({ b, bpm });
			}
		}
	}

	std::sort(
	  bpms.begin(), bpms.end(), [](const BpmEntry& a, const BpmEntry& b) {
		  return a.beat < b.beat;
	  });

	float initialBpm = 120.0f;
	if (!bpms.empty()) {
		initialBpm = bpms[0].bpm;
		if (bpms[0].beat > 0.0) {
			timing.AddSegment(BPMSegment(0, bpms[0].bpm));
		}
		for (const auto& pt : bpms) {
			int row = BeatToNoteRow(static_cast<float>(pt.beat));
			timing.AddSegment(BPMSegment(row, pt.bpm));
		}
	} else {
		timing.AddSegment(BPMSegment(0, 120.0f));
	}

	// Parse effect (scroll speed changes)
	if (doc.HasMember("effect") && doc["effect"].IsArray()) {
		for (const auto& eff : doc["effect"].GetArray()) {
			if (!eff.IsObject() || !eff.HasMember("beat") ||
				!eff.HasMember("scroll")) {
				continue;
			}
			if (!eff["scroll"].IsNumber()) {
				continue;
			}
			double b = ParseBeat(eff["beat"]);
			float scroll = static_cast<float>(eff["scroll"].GetDouble());
			int row = BeatToNoteRow(static_cast<float>(b));
			timing.AddSegment(ScrollSegment(row, scroll));
		}
	}

	float offsetSec = soundOffsetMs / 1000.0f;
	float soundBeatSec = static_cast<float>(soundBeat * 60.0 / initialBpm);
	timing.m_fBeat0OffsetInSeconds = soundBeatSec - offsetSec;
}

static void
FindBackgroundAndVideo(const rapidjson::Document& doc,
					   const std::string& songDir,
					   std::string& backgroundOut,
					   std::string& videoOut)
{
	backgroundOut.clear();
	videoOut.clear();

	if (doc.HasMember("meta") && doc["meta"].IsObject()) {
		const auto& meta = doc["meta"];
		if (meta.HasMember("background") && meta["background"].IsString()) {
			backgroundOut = meta["background"].GetString();
		} else if (meta.HasMember("bg") && meta["bg"].IsString()) {
			backgroundOut = meta["bg"].GetString();
		} else if (meta.HasMember("cover") && meta["cover"].IsString()) {
			backgroundOut = meta["cover"].GetString();
		} else if (meta.HasMember("picture") && meta["picture"].IsString()) {
			backgroundOut = meta["picture"].GetString();
		} else if (meta.HasMember("jacket") && meta["jacket"].IsString()) {
			backgroundOut = meta["jacket"].GetString();
		}

		if (backgroundOut.empty() && meta.HasMember("song") && meta["song"].IsObject()) {
			const auto& songObj = meta["song"];
			if (songObj.HasMember("background") && songObj["background"].IsString()) {
				backgroundOut = songObj["background"].GetString();
			} else if (songObj.HasMember("bg") && songObj["bg"].IsString()) {
				backgroundOut = songObj["bg"].GetString();
			}
		}

		if (meta.HasMember("video") && meta["video"].IsString()) {
			videoOut = meta["video"].GetString();
		} else if (meta.HasMember("bga") && meta["bga"].IsString()) {
			videoOut = meta["bga"].GetString();
		} else if (meta.HasMember("movie") && meta["movie"].IsString()) {
			videoOut = meta["movie"].GetString();
		}
	}

	// Fallback for background: scan directory for image files
	if (backgroundOut.empty()) {
		const auto& img_exts = ActorUtil::GetTypeExtensionList(FT_Bitmap);
		for (const auto& ext : img_exts) {
			std::vector<std::string> matches;
			FILEMAN->GetDirListing(songDir + "*background*." + ext, matches, ONLY_FILE);
			if (matches.empty()) {
				FILEMAN->GetDirListing(songDir + "*bg*." + ext, matches, ONLY_FILE);
			}
			if (!matches.empty()) {
				backgroundOut = matches[0];
				break;
			}
		}
		if (backgroundOut.empty()) {
			for (const auto& ext : img_exts) {
				std::vector<std::string> matches;
				FILEMAN->GetDirListing(songDir + "*." + ext, matches, ONLY_FILE);
				if (!matches.empty()) {
					backgroundOut = matches[0];
					break;
				}
			}
		}
	}

	// Fallback for video: scan directory for movie files
	if (videoOut.empty()) {
		const auto& vid_exts = ActorUtil::GetTypeExtensionList(FT_Movie);
		for (const auto& ext : vid_exts) {
			std::vector<std::string> matches;
			FILEMAN->GetDirListing(songDir + "*." + ext, matches, ONLY_FILE);
			if (!matches.empty()) {
				videoOut = matches[0];
				break;
			}
		}
	}
}

static void
SetMetadata(const rapidjson::Document& doc,
			Song& out,
			const std::string& songDir,
			const std::string& musicFile)
{
	std::string title;
	std::string titleTranslit;
	std::string artist;
	std::string artistTranslit;
	std::string background;
	std::string video;

	if (doc.HasMember("meta") && doc["meta"].IsObject()) {
		const auto& meta = doc["meta"];
		if (meta.HasMember("song") && meta["song"].IsObject()) {
			const auto& songObj = meta["song"];
			if (songObj.HasMember("title") && songObj["title"].IsString()) {
				title = songObj["title"].GetString();
			}
			if (songObj.HasMember("titleorg") &&
				songObj["titleorg"].IsString()) {
				titleTranslit = songObj["titleorg"].GetString();
			}
			if (songObj.HasMember("artist") && songObj["artist"].IsString()) {
				artist = songObj["artist"].GetString();
			}
			if (songObj.HasMember("artistorg") &&
				songObj["artistorg"].IsString()) {
				artistTranslit = songObj["artistorg"].GetString();
			}
		}
		if (title.empty() && meta.HasMember("title") &&
			meta["title"].IsString()) {
			title = meta["title"].GetString();
		}
		if (artist.empty() && meta.HasMember("artist") &&
			meta["artist"].IsString()) {
			artist = meta["artist"].GetString();
		}
		if (meta.HasMember("preview") && meta["preview"].IsNumber()) {
			out.m_fMusicSampleStartSeconds =
			  static_cast<float>(meta["preview"].GetDouble() / 1000.0);
			out.m_fMusicSampleLengthSeconds = 12.0f;
		}
	}

	FindBackgroundAndVideo(doc, songDir, background, video);

	if (title.empty()) {
		title = Basename(songDir);
	}
	if (artist.empty()) {
		artist = "Unknown Artist";
	}

	out.m_sMainTitle = title;
	out.m_sMainTitleTranslit = titleTranslit;
	out.m_sArtist = artist;
	out.m_sArtistTranslit = artistTranslit;
	ConvertString(out.m_sMainTitle, "utf-8,english");
	ConvertString(out.m_sArtist, "utf-8,english");

	out.m_sMusicFile = musicFile;
	if (!background.empty()) {
		out.m_sBackgroundFile = background;
		// Add background change at beat 0 so gameplay displays the song background
		out.AddBackgroundChange(
		  BACKGROUND_LAYER_1,
		  BackgroundChange(0, background, "", 1.f, SBE_StretchNormal));
	}
	if (!video.empty()) {
		out.m_sPreviewVidFile = video;
		out.AddBackgroundChange(
		  BACKGROUND_LAYER_1,
		  BackgroundChange(0, video, "", 1.f, SBE_StretchNoLoop));
	}
	out.m_DisplayBPMType = DISPLAY_BPM_ACTUAL;
}

static void
LoadNoteDataFromParsedData(Steps* chart, const rapidjson::Document& doc)
{
	if (!doc.HasMember("meta") || !doc["meta"].IsObject()) {
		return;
	}
	const auto& meta = doc["meta"];
	if (!meta.HasMember("mode_ext") || !meta["mode_ext"].IsObject()) {
		return;
	}
	const auto& mode_ext = meta["mode_ext"];
	if (!mode_ext.HasMember("column") || !mode_ext["column"].IsInt()) {
		return;
	}
	int numColumns = mode_ext["column"].GetInt();
	if (numColumns <= 0) {
		return;
	}

	NoteData noteData;
	noteData.SetNumTracks(numColumns);

	if (doc.HasMember("note") && doc["note"].IsArray()) {
		const auto& notes = doc["note"];
		for (const auto& n : notes.GetArray()) {
			if (!n.IsObject()) {
				continue;
			}
			if (!n.HasMember("column") || !n["column"].IsInt()) {
				continue;
			}
			int col = n["column"].GetInt();
			if (col < 0 || col >= numColumns) {
				continue;
			}
			if (!n.HasMember("beat")) {
				continue;
			}

			double startBeat = ParseBeat(n["beat"]);
			int startRow = BeatToNoteRow(static_cast<float>(startBeat));
			if (startRow < 0) {
				continue;
			}

			if (n.HasMember("endbeat")) {
				double endBeat = ParseBeat(n["endbeat"]);
				int endRow = BeatToNoteRow(static_cast<float>(endBeat));
				if (endRow > startRow) {
					noteData.AddHoldNote(
					  col, startRow, endRow, TAP_ORIGINAL_HOLD_HEAD);
				} else {
					noteData.SetTapNote(col, startRow, TAP_ORIGINAL_TAP);
				}
			} else {
				noteData.SetTapNote(col, startRow, TAP_ORIGINAL_TAP);
			}
		}
	}

	chart->SetNoteData(noteData);
}

void
GetApplicableFiles(const std::string& sPath, std::vector<std::string>& out)
{
	FILEMAN->GetDirListing(sPath + std::string("*.mc"), out, ONLY_FILE);
}

bool
LoadNoteDataFromSimfile(const std::string& path, Steps& out)
{
	RageFile f;
	if (!f.Open(path)) {
		return false;
	}

	std::string fileContents;
	fileContents.reserve(f.GetFileSize());
	f.Read(fileContents, -1);

	rapidjson::Document doc;
	if (doc.Parse(fileContents.c_str()).HasParseError() || !doc.IsObject()) {
		return false;
	}

	LoadNoteDataFromParsedData(&out, doc);
	return !out.IsNoteDataEmpty();
}

bool
LoadFromDir(const std::string& sPath_, Song& out)
{
	std::vector<std::string> aFileNames;
	GetApplicableFiles(sPath_, aFileNames);
	if (aFileNames.empty()) {
		return false;
	}

	RageFile f;
	bool loadedAny = false;

	for (const auto& filename : aFileNames) {
		std::string p = sPath_ + filename;

		if (!f.Open(p)) {
			continue;
		}
		std::string fileContents;
		fileContents.reserve(f.GetFileSize());
		f.Read(fileContents, -1);

		rapidjson::Document doc;
		if (doc.Parse(fileContents.c_str()).HasParseError() ||
			!doc.IsObject()) {
			continue;
		}

		if (!doc.HasMember("meta") || !doc["meta"].IsObject()) {
			continue;
		}
		const auto& meta = doc["meta"];
		if (!meta.HasMember("mode") || !meta["mode"].IsInt() ||
			meta["mode"].GetInt() != 0) {
			continue;
		}

		if (!meta.HasMember("mode_ext") || !meta["mode_ext"].IsObject()) {
			continue;
		}
		const auto& mode_ext = meta["mode_ext"];
		if (!mode_ext.HasMember("column") || !mode_ext["column"].IsInt()) {
			continue;
		}
		int columns = mode_ext["column"].GetInt();
		StepsType st = GetStepsType(columns);
		if (st == StepsType_Invalid) {
			continue;
		}

		std::string musicFile;
		double soundBeat = 0.0;
		float soundOffsetMs = 0.0f;
		FindSoundFileAndOffset(doc, sPath_, musicFile, soundBeat, soundOffsetMs);

		if (out.m_SongTiming.empty()) {
			SetMetadata(doc, out, sPath_, musicFile);
			SetTimingData(doc, out.m_SongTiming, soundBeat, soundOffsetMs);
		} else if (out.m_sBackgroundFile.empty()) {
			std::string background, video;
			FindBackgroundAndVideo(doc, sPath_, background, video);
			if (!background.empty()) {
				out.m_sBackgroundFile = background;
				out.AddBackgroundChange(
				  BACKGROUND_LAYER_1,
				  BackgroundChange(0, background, "", 1.f, SBE_StretchNormal));
			}
			if (!video.empty()) {
				out.m_sPreviewVidFile = video;
				out.AddBackgroundChange(
				  BACKGROUND_LAYER_1,
				  BackgroundChange(0, video, "", 1.f, SBE_StretchNoLoop));
			}
		}

		Steps* chart = out.CreateSteps();
		chart->SetFilename(p);
		chart->m_StepsType = st;

		if (meta.HasMember("creator") && meta["creator"].IsString()) {
			chart->SetCredit(meta["creator"].GetString());
		}
		std::string version = "";
		if (meta.HasMember("version") && meta["version"].IsString()) {
			version = meta["version"].GetString();
		}
		chart->SetDescription(version);

		Difficulty diff = OldStyleStringToDifficulty(version);
		if (diff == Difficulty_Invalid) {
			diff = StringToDifficulty(version);
		}
		if (diff == Difficulty_Invalid) {
			diff = static_cast<Difficulty>(std::min(
			  out.GetAllSteps().size(), static_cast<size_t>(Difficulty_Edit)));
		}
		chart->SetDifficulty(diff);
		chart->SetMeter(static_cast<int>(out.GetAllSteps().size() + 1));

		// If this chart has scroll effects, configure per-steps timing
		if (doc.HasMember("effect") && doc["effect"].IsArray() &&
			doc["effect"].Size() > 0) {
			SetTimingData(doc, chart->m_Timing, soundBeat, soundOffsetMs);
		}

		LoadNoteDataFromParsedData(chart, doc);

		chart->TidyUpData();
		chart->SetSavedToDisk(true);

		out.AddSteps(chart);
		loadedAny = true;
	}

	if (loadedAny) {
		out.m_sSongFileName = sPath_ + aFileNames[0];
	}

	return loadedAny;
}

} // namespace MalodyLoader
