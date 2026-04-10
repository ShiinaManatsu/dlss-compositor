#include <cstdio>
#include <cstring>

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("dlss-compositor v0.1.0\n");
            printf("Usage: dlss-compositor [options]\n");
            printf("Options:\n");
            printf("  --input-dir <dir>     Input EXR sequence directory\n");
            printf("  --output-dir <dir>    Output EXR sequence directory\n");
            printf("  --scale <factor>      Upscale factor (default: 2)\n");
            printf("  --quality <mode>      DLSS quality mode (MaxQuality|Balanced|Performance|UltraPerformance)\n");
            printf("  --channel-map <file>  Custom channel name mapping JSON file\n");
            printf("  --encode-video [file] Encode output sequence to MP4 via FFmpeg\n");
            printf("  --fps <rate>          Frame rate for video encoding (default: 24)\n");
            printf("  --test-ngx            Test NGX/DLSS-RR availability and exit\n");
            printf("  --gui                 Launch ImGui viewer\n");
            printf("  --test-gui            Non-interactive GUI smoke test (exit after 5 frames)\n");
            printf("  --help                Show this help\n");
            printf("  --version             Show version\n");
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("dlss-compositor 0.1.0\n");
            return 0;
        }
        if (strcmp(argv[i], "--test-ngx") == 0) {
            printf("--test-ngx: NGX support not yet implemented (placeholder)\n");
            return 0;
        }
    }
    printf("dlss-compositor: no arguments. Use --help for usage.\n");
    return 0;
}
