// ignore_for_file: non_constant_identifier_names

import 'dart:ffi';

import 'package:ffi/ffi.dart';
import 'package:win32/win32.dart';

/// Opens a Win32 [IFileOpenDialog] and returns the selected file path,
/// or `null` if the user cancelled or an error occurred.
///
/// [filterDescription] is the human-readable label shown in the filter list.
/// [filterPattern] is the wildcard pattern, e.g. `*.json`.
String? selectFile({
  String filterDescription = 'DLSS Config',
  String filterPattern = '*.json',
}) {
  // Initialize COM on the calling thread (safe to call multiple times).
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  final dialog = FileOpenDialog.createInstance();

  try {
    // Set file type filter.
    final filterSpec = calloc<COMDLG_FILTERSPEC>();
    try {
      filterSpec.ref.pszName = filterDescription.toNativeUtf16();
      filterSpec.ref.pszSpec = filterPattern.toNativeUtf16();
      dialog.setFileTypes(1, filterSpec);
    } finally {
      // The UTF-16 strings are owned by setFileTypes; free after the call.
      calloc.free(filterSpec.ref.pszName);
      calloc.free(filterSpec.ref.pszSpec);
      calloc.free(filterSpec);
    }

    final hr = dialog.show(0);
    if (FAILED(hr)) return null;

    final ppsi = calloc<Pointer<COMObject>>();
    try {
      final hrResult = dialog.getResult(ppsi);
      if (FAILED(hrResult)) return null;

      final shellItem = IShellItem(ppsi.value);
      try {
        final ppszName = calloc<Pointer<Utf16>>();
        try {
          final hrName =
              shellItem.getDisplayName(SIGDN_FILESYSPATH, ppszName);
          if (FAILED(hrName)) return null;

          final path = ppszName.value.toDartString();
          CoTaskMemFree(ppszName.value);
          return path;
        } finally {
          calloc.free(ppszName);
        }
      } finally {
        shellItem.release();
      }
    } finally {
      calloc.free(ppsi);
    }
  } finally {
    dialog.release();
    CoUninitialize();
  }
}

/// Opens a Win32 [IFileOpenDialog] in folder-picker mode and returns the
/// selected directory path, or `null` if cancelled or an error occurred.
String? selectDirectory() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  final dialog = FileOpenDialog.createInstance();

  try {
    dialog.setOptions(FOS_PICKFOLDERS);

    final hr = dialog.show(0);
    if (FAILED(hr)) return null;

    final ppsi = calloc<Pointer<COMObject>>();
    try {
      final hrResult = dialog.getResult(ppsi);
      if (FAILED(hrResult)) return null;

      final shellItem = IShellItem(ppsi.value);
      try {
        final ppszName = calloc<Pointer<Utf16>>();
        try {
          final hrName =
              shellItem.getDisplayName(SIGDN_FILESYSPATH, ppszName);
          if (FAILED(hrName)) return null;

          final path = ppszName.value.toDartString();
          CoTaskMemFree(ppszName.value);
          return path;
        } finally {
          calloc.free(ppszName);
        }
      } finally {
        shellItem.release();
      }
    } finally {
      calloc.free(ppsi);
    }
  } finally {
    dialog.release();
    CoUninitialize();
  }
}
