#include "global.h"

namespace demo {

using namespace dsp::audio::synth::effects;

void IniFile::Section::Set(const std::string& key, double value) {
  char text[64];
  snprintf(text, sizeof(text), "%.6g", value);
  values.emplace_back(key, text);
}

const std::string* IniFile::Section::Get(const std::string& key) const {
  for (auto& kv : values)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

double IniFile::Section::GetNumber(const std::string& key, double fallback) const {
  auto v = Get(key);
  if (!v || v->empty()) return fallback;
  char* end = nullptr;
  const double d = strtod(v->c_str(), &end);
  return end == v->c_str() ? fallback : d;
}

IniFile::Section& IniFile::Add(const std::string& name) {
  sections.push_back(Section{name, {}});
  return sections.back();
}

const IniFile::Section* IniFile::Find(const std::string& name) const {
  for (auto& s : sections)
    if (s.name == name) return &s;
  return nullptr;
}

static std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return s.substr(a, b - a);
}

bool IniFile::Load(const char* path) {
  sections.clear();
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f) return false;
  Section* current = &Add("");  // keys before the first [section]
  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    std::string s = Trim(line);
    if (s.empty() || s[0] == '#' || s[0] == ';') continue;
    if (s.front() == '[' && s.back() == ']') {
      current = &Add(Trim(s.substr(1, s.size() - 2)));
      continue;
    }
    const size_t eq = s.find('=');
    if (eq == std::string::npos) continue;
    current->Set(Trim(s.substr(0, eq)), Trim(s.substr(eq + 1)));
  }
  fclose(f);
  return true;
}

bool IniFile::Save(const char* path, const char* header_comment) const {
  // Write to a temporary file first so a failed save never destroys the old file.
  std::string tmp = std::string(path) + ".tmp";
  FILE* f = nullptr;
  if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f) return false;
  if (header_comment) fprintf(f, "# %s\r\n", header_comment);
  for (auto& s : sections) {
    if (s.name.empty() && s.values.empty()) continue;
    if (!s.name.empty()) fprintf(f, "\r\n[%s]\r\n", s.name.c_str());
    for (auto& kv : s.values) fprintf(f, "%s = %s\r\n", kv.first.c_str(), kv.second.c_str());
  }
  const bool ok = fflush(f) == 0 && !ferror(f);
  fclose(f);
  if (!ok || !MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileA(tmp.c_str());
    return false;
  }
  return true;
}

static bool IsChainParam(int id) {
  return id != kStripVolume && id != kStripPan && id != kStripMute && id != kStripSolo;
}

void WriteStripParams(IniFile::Section& section, const StripParams& params, bool chain_only) {
  for (int id = 0; id < kStripParamCount; ++id) {
    if (chain_only && !IsChainParam(id)) continue;
    const float v = params.get(id);
    if (v != StripParamInfo(id).def) section.Set(StripParamKey(id), v);
  }
}

void ReadStripParams(const IniFile::Section& section, StripParams& params, bool chain_only) {
  for (int id = 0; id < kStripParamCount; ++id)
    if (!chain_only || IsChainParam(id)) params.set(id, StripParamInfo(id).def);
  for (auto& kv : section.values) {
    const int id = FindStripParam(kv.first.c_str());
    if (id < 0 || (chain_only && !IsChainParam(id))) continue;
    const auto& info = StripParamInfo(id);
    params.set(id, std::clamp(float(atof(kv.second.c_str())), info.min, info.max));
  }
}

void WriteMasterParams(IniFile::Section& section, const MasterParams& params) {
  for (int id = 0; id < kMasterParamCount; ++id) {
    const float v = params.get(id);
    if (v != MasterParamInfo(id).def) section.Set(MasterParamKey(id), v);
  }
}

void ReadMasterParams(const IniFile::Section& section, MasterParams& params) {
  for (int id = 0; id < kMasterParamCount; ++id) params.set(id, MasterParamInfo(id).def);
  for (auto& kv : section.values) {
    const int id = FindMasterParam(kv.first.c_str());
    if (id < 0) continue;
    const auto& info = MasterParamInfo(id);
    params.set(id, std::clamp(float(atof(kv.second.c_str())), info.min, info.max));
  }
}

}
