#ifndef NOTES_LOADER_MALODY_H
#define NOTES_LOADER_MALODY_H

#include <string>
#include <vector>

class Song;
class Steps;

namespace MalodyLoader {

void
GetApplicableFiles(const std::string& sPath, std::vector<std::string>& out);

bool
LoadFromDir(const std::string& sPath, Song& out);

bool
LoadNoteDataFromSimfile(const std::string& path, Steps& out);

} // namespace MalodyLoader

#endif
