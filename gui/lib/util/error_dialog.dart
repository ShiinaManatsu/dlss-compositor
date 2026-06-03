import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

Future<void> showErrorDialog(
  BuildContext context, {
  required String title,
  required String message,
  String? details,
}) async {
  final copyText = details == null || details.isEmpty
      ? '$title\n$message'
      : '$title\n$message\n\n$details';

  await showDialog<void>(
    context: context,
    builder: (dialogContext) => AlertDialog(
      title: Text(title),
      content: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 520),
        child: SelectableText(
          details == null || details.isEmpty ? message : '$message\n\n$details',
        ),
      ),
      actions: [
        TextButton.icon(
          onPressed: () {
            Clipboard.setData(ClipboardData(text: copyText));
            ScaffoldMessenger.of(context).showSnackBar(
              const SnackBar(content: Text('Copied error details')),
            );
          },
          icon: const Icon(Icons.copy, size: 16),
          label: const Text('Copy'),
        ),
        FilledButton(
          onPressed: () => Navigator.of(dialogContext).pop(),
          child: const Text('OK'),
        ),
      ],
    ),
  );
}
