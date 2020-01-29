#include<string>
#include "yaml-cpp/eventhandler.h"
#include "yaml-cpp/yaml.h"

struct ScoutTest {
    std::vector<std::string> wasm_binaries;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> shard_blocks;
    std::vector<std::vector<uint8_t>> pre_states;
    std::vector<std::vector<uint8_t>> post_states;
};

ScoutTest load_scout_config(std::string &yaml_file) {
    auto s = ScoutTest();

    YAML::Node yaml = YAML::LoadFile(yaml_file);
    YAML::Node yaml_execution_scripts = yaml["beacon_state"]["execution_scripts"];

    for (std::size_t i=0;i<yaml_execution_scripts.size();i++) {
        s.wasm_binaries.push_back(yaml_execution_scripts[i].as<std::string>());
    }

    return s;
}
