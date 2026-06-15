import 'package:filepicker_windows/filepicker_windows.dart';

/// Opens a Win32 [IFileOpenDialog] and returns the selected file path,
/// or `null` if the user cancelled or an error occurred.
///
/// [filterDescription] is the human-readable label shown in the filter list.
/// [filterPattern] is the wildcard pattern, e.g. `*.json`.
String? selectFile({
  String filterDescription = 'DLSS Config',
  String filterPattern = '*.json',
  String title = 'Open dlssconfig.json',
}) {
  final picker = OpenFilePicker()
    ..filterSpecification = {filterDescription: filterPattern}
    ..defaultFilterIndex = 0
    ..defaultExtension = filterPattern.replaceFirst('*.', '')
    ..fileMustExist = true
    ..forceFileSystemItems = true
    ..title = title;

  return picker.getFile()?.path;
}

/// Opens a Win32 [IFileOpenDialog] in folder-picker mode and returns the
/// selected directory path, or `null` if cancelled or an error occurred.
String? selectDirectory({String title = 'Select output directory'}) {
  final picker = DirectoryPicker()
    ..forceFileSystemItems = true
    ..title = title;

  return picker.getDirectory()?.path;
}
