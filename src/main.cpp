#include <cstdio>
#include <string>

#include "cli/cli_parser.h"
#include "cli/config.h"

int main(int argc, char* argv[]) {
    AppConfig config;
    std::string errorMsg;

    if (!CliParser::parse(argc, argv, config, errorMsg)) {
        fprintf(stderr, "Error: %s\n", errorMsg.c_str());
        fprintf(stderr, "Run with --help for usage.\n");
        return 1;
    }

    if (config.showHelp) {
        CliParser::printHelp();
        return 0;
    }
    if (config.showVersion) {
        CliParser::printVersion();
        return 0;
    }
    if (config.testNgx) {
        printf("--test-ngx: NGX support not yet implemented (placeholder)\n");
        return 0;
    }

    printf("dlss-compositor: no action specified. Use --help for usage.\n");
    return 0;
}
