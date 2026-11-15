#pragma once

// Session files (.dspsession) and processing-chain presets (.dspchain / .dspbus).
// Plain INI text: "[section]" lines and "key = value" lines; '#' starts a comment. Mixer
// parameters are written by their stable keys (see StripParamKey) and only when they differ
// from the default, so files stay short and survive new parameters being added.

#include <string>
#include <vector>

namespace demo {

class IniFile {
 public:
  struct Section {
    std::string name;
    std::vector<std::pair<std::string, std::string>> values;
    void Set(const std::string& key, const std::string& value) { values.emplace_back(key, value); }
    void Set(const std::string& key, double value);
    const std::string* Get(const std::string& key) const;
    double GetNumber(const std::string& key, double fallback) const;
  };
  std::vector<Section> sections;

  Section& Add(const std::string& name);
  const Section* Find(const std::string& name) const;
  bool Load(const char* path);
  bool Save(const char* path, const char* header_comment) const;
};

// Mixer parameters <-> INI section. chain_only leaves out the fader, pan, mute and solo
// (a chain preset carries the processing, not the channel's place in the mix).
void WriteStripParams(IniFile::Section& section, const dsp::audio::synth::effects::StripParams& params, bool chain_only);
void ReadStripParams(const IniFile::Section& section, dsp::audio::synth::effects::StripParams& params, bool chain_only);
void WriteMasterParams(IniFile::Section& section, const dsp::audio::synth::effects::MasterParams& params);
void ReadMasterParams(const IniFile::Section& section, dsp::audio::synth::effects::MasterParams& params);

}
