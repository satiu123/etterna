#include "Etterna/Globals/global.h"
#include "NotesLoaderMALODY.h"
#include "Etterna/Actor/Base/ActorUtil.h"
#include "Etterna/Models/Misc/BackgroundUtil.h"
#include "Etterna/Models/Misc/Difficulty.h"
#include "Etterna/Models/NoteData/NoteData.h"
#include "Etterna/Models/Songs/Song.h"
#include "Etterna/Models/StepsAndStyles/Steps.h"
#include "Etterna/Singletons/GameManager.h"
#include "Etterna/Singletons/PrefsManager.h"
#include "Etterna/Globals/SpecialFiles.h"
#include "Etterna/Models/Songs/SongCacheIndex.h"
#include "Core/Services/Locator.hpp"
#include "RageUtil/File/RageFile.h"
#include "RageUtil/File/RageFileManager.h"
#include "RageUtil/Utils/RageUtil.h"
#include "RageUtil/Utils/RageUtil_CharConversions.h"

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"

#include <ghc/filesystem.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
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

	bool foundSound = false;

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
					foundSound = true;
					break;
				}
			}
		}
		// Priority 2: audio file extension
		if (!foundSound) {
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
						foundSound = true;
						break;
					}
				}
			}
		}
		// Priority 3: any sound note
		if (!foundSound) {
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
					foundSound = true;
					break;
				}
			}
		}
	}

	// Fallback/additive offset check in meta or root
	if (doc.HasMember("meta") && doc["meta"].IsObject()) {
		const auto& meta = doc["meta"];
		if (meta.HasMember("offset") && meta["offset"].IsNumber()) {
			soundOffsetMsOut += static_cast<float>(meta["offset"].GetDouble());
		} else if (meta.HasMember("song") && meta["song"].IsObject() &&
		           meta["song"].HasMember("offset") && meta["song"]["offset"].IsNumber()) {
			soundOffsetMsOut += static_cast<float>(meta["song"]["offset"].GetDouble());
		}
	}
	if (doc.HasMember("offset") && doc["offset"].IsNumber()) {
		soundOffsetMsOut += static_cast<float>(doc["offset"].GetDouble());
	}

	if (foundSound) {
		return true;
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

	if (!bpms.empty()) {
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

	double soundBeatSec = 0.0;
	if (soundBeat > 0.0 && !bpms.empty()) {
		double currentBeat = 0.0;
		double currentBpm = bpms[0].bpm;
		for (size_t i = 0; i < bpms.size(); ++i) {
			if (bpms[i].beat >= soundBeat) {
				break;
			}
			if (bpms[i].beat > currentBeat) {
				soundBeatSec += (bpms[i].beat - currentBeat) * (60.0 / currentBpm);
				currentBeat = bpms[i].beat;
			}
			currentBpm = bpms[i].bpm;
		}
		if (soundBeat > currentBeat) {
			soundBeatSec += (soundBeat - currentBeat) * (60.0 / currentBpm);
		}
	} else if (soundBeat > 0.0) {
		soundBeatSec = soundBeat * (60.0 / 120.0);
	}

	float offsetSec = soundOffsetMs / 1000.0f;
	timing.m_fBeat0OffsetInSeconds = static_cast<float>(soundBeatSec) + offsetSec;
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
	std::string version;
	std::string creator;

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
		if (meta.HasMember("version") && meta["version"].IsString()) {
			version = meta["version"].GetString();
		}
		if (meta.HasMember("creator") && meta["creator"].IsString()) {
			creator = meta["creator"].GetString();
		}
		if (meta.HasMember("preview") && meta["preview"].IsNumber()) {
			out.m_fMusicSampleStartSeconds =
			  static_cast<float>(meta["preview"].GetDouble() / 1000.0);
			out.m_fMusicSampleLengthSeconds = 12.0f;
		}
	}

	FindBackgroundAndVideo(doc, songDir, background, video);

	// Dan course / Compilation song title and artist refinement:
	std::string lowerArtist = make_lower(artist);
	bool isCompilation = (lowerArtist == "various artists" ||
						  lowerArtist == "various" ||
						  artist.empty() ||
						  artist == "Unknown Artist");

	// If audio filename has "Artist - Title", try extracting
	std::string audioStem = Basename(musicFile);
	size_t extDot = audioStem.rfind('.');
	if (extDot != std::string::npos) {
		audioStem = audioStem.substr(0, extDot);
	}

	size_t dashPos = audioStem.find(" - ");
	if (dashPos != std::string::npos) {
		std::string audioArtist = audioStem.substr(0, dashPos);
		std::string audioTitle = audioStem.substr(dashPos + 3);
		size_t a1 = audioArtist.find_first_not_of(" ");
		size_t a2 = audioArtist.find_last_not_of(" ");
		if (a1 != std::string::npos) {
			audioArtist = audioArtist.substr(a1, a2 - a1 + 1);
		}
		size_t t1 = audioTitle.find_first_not_of(" ");
		size_t t2 = audioTitle.find_last_not_of(" ");
		if (t1 != std::string::npos) {
			audioTitle = audioTitle.substr(t1, t2 - t1 + 1);
		}

		if (isCompilation || title == out.m_sGroupName) {
			if (!audioArtist.empty()) {
				artist = audioArtist;
			}
			if (!audioTitle.empty()) {
				title = audioTitle;
			}
		}
	} else if (isCompilation && !version.empty()) {
		title = version;
		if (!creator.empty()) {
			artist = creator;
		}
	}

	// Extract Dan level / Subtitle from version
	if (!version.empty()) {
		std::string danPrefix;
		std::string rateMod;
		size_t openParen = version.find('(');
		size_t closeParen = version.find(')', openParen);
		if (openParen != std::string::npos && closeParen != std::string::npos &&
			closeParen > openParen) {
			std::string parenContent =
			  version.substr(openParen, closeParen - openParen + 1);
			if (parenContent.find('x') != std::string::npos ||
				parenContent.find('X') != std::string::npos) {
				rateMod = parenContent;
			}
		}

		std::string lowerVer = make_lower(version);
		if (lowerVer.rfind("extra", 0) == 0 ||
			lowerVer.rfind("reg", 0) == 0 ||
			lowerVer.rfind("dan", 0) == 0) {
			size_t spacePos = version.find(' ');
			if (spacePos != std::string::npos) {
				danPrefix = version.substr(0, spacePos);
			} else {
				danPrefix = version;
			}
			if (!rateMod.empty() && danPrefix.find(rateMod) == std::string::npos) {
				danPrefix += " " + rateMod;
			}
		} else if (!rateMod.empty()) {
			danPrefix = rateMod;
		}

		if (!danPrefix.empty() && danPrefix != title) {
			out.m_sSubTitle = danPrefix;
		}
	}

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
	ConvertString(out.m_sSubTitle, "utf-8,english");
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

		TimingData chartTiming;
		SetTimingData(doc, chartTiming, soundBeat, soundOffsetMs);

		if (out.m_SongTiming.empty()) {
			SetMetadata(doc, out, sPath_, musicFile);
			out.m_SongTiming = chartTiming;
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
			if (out.GetAllSteps().empty()) {
				diff = Difficulty_Challenge;
			} else {
				diff = static_cast<Difficulty>(std::min(
				  out.GetAllSteps().size(), static_cast<size_t>(Difficulty_Edit)));
			}
		}
		chart->SetDifficulty(diff);
		chart->SetMeter(static_cast<int>(out.GetAllSteps().size() + 1));

		// If this chart's timing differs from the song's default timing
		// (e.g. scroll effects, per-chart offset or BPM changes), configure per-steps timing
		if (chartTiming != out.m_SongTiming) {
			chart->m_Timing = chartTiming;
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

static inline std::string
SanitizeFileName(const std::string& input)
{
	std::string result;
	result.reserve(input.size());
	for (char c : input) {
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
			c == '"' || c == '<' || c == '>' || c == '|' ||
			static_cast<unsigned char>(c) < 32 || c == 127) {
			result += '_';
		} else {
			result += c;
		}
	}
	size_t start = result.find_first_not_of(" ._");
	if (start == std::string::npos) {
		return "";
	}
	size_t end = result.find_last_not_of(" ._");
	return result.substr(start, end - start + 1);
}

static bool
TryUnpackDanDirectory(const std::string& baseDir, const std::string& folderName)
{
	std::string folderPath = baseDir + folderName;
	if (folderPath.empty() || folderPath.back() != '/') {
		folderPath += "/";
	}

	std::string checkDir = folderPath;
	std::string subFolderName = "";
	std::vector<std::string> mcFiles;
	FILEMAN->GetDirListing(checkDir + "*.mc", mcFiles, ONLY_FILE);

	if (mcFiles.empty()) {
		std::vector<std::string> subdirs;
		FILEMAN->GetDirListing(checkDir + "*", subdirs, ONLY_DIR);
		for (const auto& sd : subdirs) {
			std::string candidateDir = checkDir + sd + "/";
			std::vector<std::string> candidateMcs;
			FILEMAN->GetDirListing(candidateDir + "*.mc", candidateMcs, ONLY_FILE);
			if (candidateMcs.size() >= 2) {
				mcFiles = candidateMcs;
				checkDir = candidateDir;
				subFolderName = sd;
				break;
			}
		}
	}

	if (mcFiles.size() < 2) {
		return false;
	}

	struct DanSongItem
	{
		std::string mcFilename;
		std::string mcCompanion;
		std::string soundFilename;
		std::string bgFilename;
		std::string videoFilename;
		std::string title;
		std::string artist;
		std::string version;
		std::string creator;
	};

	std::vector<DanSongItem> items;
	std::set<std::string> uniqueSounds;
	std::set<std::string> uniqueTitles;
	std::string packTitle;

	for (const auto& mcName : mcFiles) {
		RageFile f;
		if (!f.Open(checkDir + mcName)) {
			continue;
		}
		std::string content;
		content.reserve(f.GetFileSize());
		f.Read(content, -1);
		rapidjson::Document doc;
		if (doc.Parse(content.c_str()).HasParseError() || !doc.IsObject()) {
			continue;
		}

		DanSongItem item;
		item.mcFilename = mcName;
		item.mcCompanion = mcName + "_";

		double soundBeat = 0.0;
		float soundOffsetMs = 0.0f;
		FindSoundFileAndOffset(doc, checkDir, item.soundFilename, soundBeat, soundOffsetMs);
		FindBackgroundAndVideo(doc, checkDir, item.bgFilename, item.videoFilename);

		if (doc.HasMember("meta") && doc["meta"].IsObject()) {
			const auto& meta = doc["meta"];
			if (meta.HasMember("song") && meta["song"].IsObject()) {
				const auto& s = meta["song"];
				if (s.HasMember("title") && s["title"].IsString()) {
					item.title = s["title"].GetString();
				}
				if (s.HasMember("artist") && s["artist"].IsString()) {
					item.artist = s["artist"].GetString();
				}
			}
			if (item.title.empty() && meta.HasMember("title") && meta["title"].IsString()) {
				item.title = meta["title"].GetString();
			}
			if (item.artist.empty() && meta.HasMember("artist") && meta["artist"].IsString()) {
				item.artist = meta["artist"].GetString();
			}
			if (meta.HasMember("version") && meta["version"].IsString()) {
				item.version = meta["version"].GetString();
			}
			if (meta.HasMember("creator") && meta["creator"].IsString()) {
				item.creator = meta["creator"].GetString();
			}
		}

		if (!item.soundFilename.empty()) {
			uniqueSounds.insert(item.soundFilename);
		}
		if (!item.title.empty()) {
			uniqueTitles.insert(item.title);
			if (packTitle.empty()) {
				packTitle = item.title;
			}
		}
		items.push_back(item);
	}

	// Malody Dan / Compilation pack criteria:
	// Multiple charts with >= 2 distinct audio files or >= 2 distinct titles
	if (items.size() < 2 || (uniqueSounds.size() < 2 && uniqueTitles.size() < 2)) {
		return false;
	}

	bool isAdditional = (baseDir.find("AdditionalSongs") != std::string::npos);
	std::string realFolderDir = FILEMAN->ResolveSongFolder(folderPath, isAdditional);
	std::string realCheckDir = FILEMAN->ResolveSongFolder(checkDir, isAdditional);
	std::string realBaseDir = FILEMAN->ResolveSongFolder(baseDir, isAdditional);

	if (realFolderDir.empty() || realCheckDir.empty() || realBaseDir.empty()) {
		return false;
	}

	ghc::filesystem::path folderFsPath = ghc::filesystem::u8path(realFolderDir);
	ghc::filesystem::path sourceFsPath = ghc::filesystem::u8path(realCheckDir);
	ghc::filesystem::path baseFsPath = ghc::filesystem::u8path(realBaseDir);

	std::string sanitizedPack = SanitizeFileName(packTitle);
	if (sanitizedPack.empty()) {
		sanitizedPack = folderName;
	}

	ghc::filesystem::path targetPackFsPath;
	bool isRootSongsDir = (baseDir == "Songs/" || baseDir == "/Songs/" ||
	                       baseDir == "AdditionalSongs/" || baseDir == "/AdditionalSongs/" ||
	                       baseDir == SpecialFiles::SONGS_DIR);
	if (isRootSongsDir) {
		targetPackFsPath = baseFsPath / sanitizedPack;
	} else {
		targetPackFsPath = baseFsPath;
	}

	std::error_code ec;
	bool renamedWholeFolder = false;

	if (isRootSongsDir && folderFsPath != targetPackFsPath) {
		if (!ghc::filesystem::exists(targetPackFsPath, ec)) {
			ghc::filesystem::rename(folderFsPath, targetPackFsPath, ec);
			if (!ec) {
				renamedWholeFolder = true;
			}
		}
	}

	if (!renamedWholeFolder) {
		ghc::filesystem::create_directories(targetPackFsPath, ec);
	}

	if (!subFolderName.empty()) {
		sourceFsPath = targetPackFsPath / subFolderName;
	} else if (renamedWholeFolder || folderFsPath == targetPackFsPath) {
		sourceFsPath = targetPackFsPath;
	}

	// Group items by soundFilename (or mcFilename if empty)
	std::map<std::string, std::vector<DanSongItem>> soundGroups;
	for (const auto& item : items) {
		std::string key = item.soundFilename.empty() ? item.mcFilename : item.soundFilename;
		soundGroups[key].push_back(item);
	}

	for (const auto& pair : soundGroups) {
		const auto& group = pair.second;
		const auto& first = group[0];

		std::string subName;
		if (!first.version.empty()) {
			subName = SanitizeFileName(first.version);
		}
		if (subName.empty() && !first.soundFilename.empty()) {
			subName = SanitizeFileName(ghc::filesystem::u8path(first.soundFilename).stem().string());
		}
		if (subName.empty()) {
			subName = SanitizeFileName(ghc::filesystem::u8path(first.mcFilename).stem().string());
		}
		if (subName.empty()) {
			subName = "Song";
		}

		ghc::filesystem::path songFsDir = targetPackFsPath / subName;
		int dupIndex = 2;
		while (ghc::filesystem::exists(songFsDir, ec) && songFsDir == sourceFsPath) {
			songFsDir = targetPackFsPath / (subName + " (" + std::to_string(dupIndex++) + ")");
		}

		ghc::filesystem::create_directories(songFsDir, ec);

		for (const auto& item : group) {
			// Move .mc
			auto srcMc = sourceFsPath / item.mcFilename;
			auto dstMc = songFsDir / item.mcFilename;
			if (ghc::filesystem::exists(srcMc, ec)) {
				ghc::filesystem::rename(srcMc, dstMc, ec);
			}
			// Move .mc_ companion
			if (!item.mcCompanion.empty()) {
				auto srcMc_ = sourceFsPath / item.mcCompanion;
				auto dstMc_ = songFsDir / item.mcCompanion;
				if (ghc::filesystem::exists(srcMc_, ec)) {
					ghc::filesystem::rename(srcMc_, dstMc_, ec);
				}
			}
			// Move sound
			if (!item.soundFilename.empty()) {
				std::string soundBase = ghc::filesystem::u8path(item.soundFilename).filename().string();
				auto srcSound = sourceFsPath / soundBase;
				auto dstSound = songFsDir / soundBase;
				if (ghc::filesystem::exists(srcSound, ec)) {
					ghc::filesystem::rename(srcSound, dstSound, ec);
				}
			}
			// Move background
			if (!item.bgFilename.empty()) {
				std::string bgBase = ghc::filesystem::u8path(item.bgFilename).filename().string();
				auto srcBg = sourceFsPath / bgBase;
				auto dstBg = songFsDir / bgBase;
				if (ghc::filesystem::exists(srcBg, ec)) {
					ghc::filesystem::rename(srcBg, dstBg, ec);
				}
			}
			// Move video
			if (!item.videoFilename.empty()) {
				std::string vidBase = ghc::filesystem::u8path(item.videoFilename).filename().string();
				auto srcVid = sourceFsPath / vidBase;
				auto dstVid = songFsDir / vidBase;
				if (ghc::filesystem::exists(srcVid, ec)) {
					ghc::filesystem::rename(srcVid, dstVid, ec);
				}
			}
		}
	}

	// Move any extra remaining files (e.g. .mc_ companions or pack banner)
	if (sourceFsPath != targetPackFsPath && ghc::filesystem::exists(sourceFsPath, ec)) {
		for (const auto& entry : ghc::filesystem::directory_iterator(sourceFsPath, ec)) {
			if (entry.is_regular_file(ec)) {
				std::string fname = entry.path().filename().string();
				std::string ext = make_lower(entry.path().extension().string());
				if (ext == ".mc_") {
					std::string targetMc = fname.substr(0, fname.length() - 1);
					for (const auto& subEntry :
					     ghc::filesystem::directory_iterator(targetPackFsPath, ec)) {
						if (subEntry.is_directory(ec) &&
						    subEntry.path() != sourceFsPath) {
							if (ghc::filesystem::exists(
							      subEntry.path() / targetMc, ec)) {
								ghc::filesystem::rename(
								  entry.path(), subEntry.path() / fname, ec);
								break;
							}
						}
					}
				} else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") {
					auto dst = targetPackFsPath / entry.path().filename();
					if (!ghc::filesystem::exists(dst, ec)) {
						ghc::filesystem::rename(entry.path(), dst, ec);
					}
				}
			}
		}
		if (ghc::filesystem::is_empty(sourceFsPath, ec)) {
			ghc::filesystem::remove(sourceFsPath, ec);
		}
	}

	if (!renamedWholeFolder && folderFsPath != targetPackFsPath && ghc::filesystem::exists(folderFsPath, ec)) {
		if (ghc::filesystem::is_empty(folderFsPath, ec)) {
			ghc::filesystem::remove(folderFsPath, ec);
		}
	}

	if (SONGINDEX != nullptr) {
		SONGINDEX->DeleteSongFromDBByDir("/" + folderPath);
		SONGINDEX->DeleteSongFromDBByDir("/" + checkDir);
		std::string fpTrim = folderPath;
		if (!fpTrim.empty() && fpTrim.back() == '/') {
			fpTrim.pop_back();
		}
		SONGINDEX->DeleteSongFromDBByDir("/" + fpTrim);
		std::string cdTrim = checkDir;
		if (!cdTrim.empty() && cdTrim.back() == '/') {
			cdTrim.pop_back();
		}
		SONGINDEX->DeleteSongFromDBByDir("/" + cdTrim);
	}

	FILEMAN->FlushDirCache(baseDir);
	FILEMAN->FlushDirCache(folderPath);
	if (isRootSongsDir) {
		FILEMAN->FlushDirCache(baseDir + sanitizedPack + "/");
	}

	Locator::getLogger()->info(
		"Unpacked Malody Dan Pack \"{}\" into \"{}\" ({} songs)",
		folderName.c_str(),
		sanitizedPack.c_str(),
		soundGroups.size());

	return true;
}

void
ProcessDanPacks(const std::string& baseDir)
{
	std::string rootDir = baseDir;
	if (rootDir.empty() || rootDir.back() != '/') {
		rootDir += "/";
	}

	std::vector<std::string> folders;
	FILEMAN->GetDirListing(rootDir + "*", folders, ONLY_DIR);

	for (const auto& folder : folders) {
		if (TryUnpackDanDirectory(rootDir, folder)) {
			continue;
		}

		std::string subDir = rootDir + folder + "/";
		std::vector<std::string> subFolders;
		FILEMAN->GetDirListing(subDir + "*", subFolders, ONLY_DIR);
		for (const auto& sf : subFolders) {
			TryUnpackDanDirectory(subDir, sf);
		}
	}
}

} // namespace MalodyLoader
