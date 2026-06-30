import 'dart:convert';
import 'dart:io';

const kDlssConfigExtension = '.dlssc';
const kLegacyDlssConfigExtension = '.json';
const kDlssConfigFileDescription = 'DLSS Compositor Config';
const kDlssConfigFileFilter = '*.dlssc;*.json';

bool isDlssConfigPath(String path) {
  final lower = path.toLowerCase();
  return lower.endsWith(kDlssConfigExtension) ||
      lower.endsWith(kLegacyDlssConfigExtension);
}

bool isBlendPath(String path) => path.toLowerCase().endsWith('.blend');

class DlssConfigLoadResult {
  const DlssConfigLoadResult({
    required this.path,
    required this.config,
    required this.error,
  });

  final String path;
  final Map<String, dynamic>? config;
  final String? error;

  bool get isValid => config != null && error == null;
}

Future<DlssConfigLoadResult> loadDlssConfigFile(
  String path, {
  required String Function(Object? got) invalidVersion,
  required String Function(String field) missingRequiredField,
}) async {
  final content = await File(path).readAsString();
  final decoded = json.decode(content);
  if (decoded is! Map<String, dynamic>) {
    return DlssConfigLoadResult(
      path: path,
      config: null,
      error: missingRequiredField('root object'),
    );
  }

  String? error;
  if (decoded['version'] != 1) {
    error = invalidVersion(decoded['version']);
  } else if (!decoded.containsKey('output_resolution')) {
    error = missingRequiredField('output_resolution');
  } else if (!decoded.containsKey('fps')) {
    error = missingRequiredField('fps');
  }

  return DlssConfigLoadResult(
    path: path,
    config: error == null ? decoded : null,
    error: error,
  );
}
