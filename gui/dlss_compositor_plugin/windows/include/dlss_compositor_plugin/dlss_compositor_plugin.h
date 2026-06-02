#ifndef FLUTTER_PLUGIN_DLSS_COMPOSITOR_PLUGIN_H_
#define FLUTTER_PLUGIN_DLSS_COMPOSITOR_PLUGIN_H_

#include <flutter_plugin_registrar.h>

#define FLUTTER_PLUGIN_EXPORT

#if defined(__cplusplus)
extern "C" {
#endif

FLUTTER_PLUGIN_EXPORT void DlssCompositorPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // FLUTTER_PLUGIN_DLSS_COMPOSITOR_PLUGIN_H_
