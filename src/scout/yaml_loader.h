#include <string>
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
    YAML::Node execution_scripts = yaml["beacon_state"]["execution_scripts"];
    YAML::Node prestates = yaml["shard_pre_state"]["exec_env_states"];
    YAML::Node shard_blocks = yaml["shard_blocks"];
    YAML::Node post_states = yaml["shard_post_state"]["exec_env_states"];

    std::vector<std::string> calldatas_str;
    std::vector<std::string> prestates_hexstr;
    std::vector<std::string> poststates_hexstr;

    // load the execution scripts
    for (std::size_t i=0; i < execution_scripts.size();i++) {
        s.wasm_binaries.push_back(execution_scripts[i].as<std::string>());
    }

    // load the prestates
    for (std::size_t i = 0; i < prestates.size(); i++) {
        prestates_hexstr.push_back(prestates[i].as<std::string>());
        s.pre_states.push_back(std::vector<uint8_t>());
        for (int j = 0 ; j < prestates_hexstr[i].size() ; j += 2) {
            auto value = ::strtol( prestates_hexstr[i].substr( j, 2 ).c_str(), 0, 16 );
            s.pre_states[i].push_back(value);
        }
    }

    // load shard blocks
    for (std::size_t i=0;i<shard_blocks.size();i++) {
        YAML::Node env = shard_blocks[i]["env"];
        YAML::Node calldata_yaml = shard_blocks[i]["data"];
        std::vector<uint8_t> calldata;
        calldatas_str.push_back(calldata_yaml.as<std::string>());
        for (int j = 0 ; j < calldatas_str[i].size() ; j+=2) {
            calldata.push_back(::strtol( calldatas_str[i].substr( j, 2 ).c_str(), 0, 16 ));
        }
        s.shard_blocks.push_back(std::make_pair(env.as<uint32_t>(),calldata));
    }

    // load post states
    s.post_states.reserve(execution_scripts.size());
    for (std::size_t i=0; i < post_states.size(); i++) {
        poststates_hexstr.push_back(post_states[i].as<std::string>());
        s.post_states.push_back(std::vector<uint8_t>());
        for (int j = 0 ; j < poststates_hexstr[i].size() ; j+=2) {
            s.post_states[i].push_back(::strtol( poststates_hexstr[i].substr( j, 2 ).c_str(), 0, 16 ));
        }
    }

    return s;
}
