#pragma once

#include "config.h"

#include <string>

class CliParser {
public:
    static bool parse(int argc, char* argv[], AppConfig& config, std::string& errorMsg);
    static void printHelp();
    static void printVersion();
};
