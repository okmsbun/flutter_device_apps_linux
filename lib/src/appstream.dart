import 'dart:convert';
import 'dart:io';
import 'dart:isolate';

import 'package:xml/xml.dart';

const _maxMetadataBytes = 1024 * 1024;
const _extensions = ['.metainfo.xml', '.appdata.xml'];

Future<Map<String, String>?> readAppStream({
  required String desktopFileId,
  required String? desktopFilePath,
  required List<String> dataDirectories,
  required List<String> languages,
}) => Isolate.run(
  () => _findMetadata(
    desktopFileId,
    desktopFilePath,
    dataDirectories,
    languages.map(_normalizeLanguage).toSet().toList(),
  ),
);

Map<String, String>? _findMetadata(
  String desktopId,
  String? desktopPath,
  List<String> dataDirectories,
  List<String> languages,
) {
  final componentId = desktopId.substring(
    0,
    desktopId.length - '.desktop'.length,
  );
  final directories = {
    ?_resolvedDataDirectory(desktopPath),
    ...dataDirectories,
  };
  for (final root in directories) {
    for (final subdirectory in ['metainfo', 'appdata']) {
      final path = '$root/$subdirectory';
      final directPaths = {
        for (final id in [componentId, desktopId])
          for (final extension in _extensions) '$path/$id$extension',
      };
      for (final candidate in directPaths) {
        final metadata = _readMetadata(candidate, desktopId, languages);
        if (metadata != null) return metadata;
      }
      try {
        final candidates =
            Directory(path)
                .listSync(followLinks: false)
                .map((entry) => entry.path)
                .where(
                  (candidate) =>
                      !directPaths.contains(candidate) &&
                      _extensions.any(candidate.endsWith),
                )
                .toList()
              ..sort();
        for (final candidate in candidates) {
          final metadata = _readMetadata(candidate, desktopId, languages);
          if (metadata != null) return metadata;
        }
      } on FileSystemException {
        continue;
      }
    }
  }
  return null;
}

String? _resolvedDataDirectory(String? desktopPath) {
  if (desktopPath == null) return null;
  try {
    final original = File(desktopPath);
    final resolvedPath = original.resolveSymbolicLinksSync();
    if (resolvedPath == desktopPath) return null;
    final parent = File(resolvedPath).parent;
    return parent.path.endsWith('/applications') ? parent.parent.path : null;
  } on FileSystemException {
    return null;
  }
}

Map<String, String>? _readMetadata(
  String path,
  String desktopId,
  List<String> languages,
) {
  try {
    final file = File(path);
    final stat = file.statSync();
    if (stat.type != FileSystemEntityType.file ||
        stat.size == 0 ||
        stat.size > _maxMetadataBytes) {
      return null;
    }
    final handle = file.openSync();
    final String text;
    try {
      final bytes = handle.readSync(_maxMetadataBytes + 1);
      if (bytes.length > _maxMetadataBytes) return null;
      text = utf8.decode(bytes);
    } finally {
      handle.closeSync();
    }
    final component = XmlDocument.parse(text).rootElement;
    if (!_matchesDesktop(component, desktopId)) return null;

    final developer = component.getElement('developer');
    final name =
        _localized(developer?.findElements('name') ?? [], languages) ??
        _localized(component.findElements('developer_name'), languages);
    final homepage = _text(
      component
          .findElements('url')
          .where((url) => url.getAttribute('type') == 'homepage')
          .firstOrNull,
    );
    final uri = homepage == null ? null : Uri.tryParse(homepage);
    final description = component.getElement('description');
    return {
      if (uri != null &&
          (uri.scheme == 'http' || uri.scheme == 'https') &&
          uri.host.isNotEmpty)
        'homepage': uri.toString(),
      'developer': ?_text(name),
      'license': ?_text(component.getElement('project_license')),
      'description': ?_description(description, languages),
    };
  } on FileSystemException {
    return null;
  } on XmlException {
    return null;
  } on FormatException {
    return null;
  }
}

bool _matchesDesktop(XmlElement component, String desktopId) {
  final kind = component.getAttribute('type');
  if (component.name.local != 'application' &&
      (component.name.local != 'component' ||
          (kind != 'desktop-application' && kind != 'desktop'))) {
    return false;
  }
  final launchables = component
      .findElements('launchable')
      .where((entry) => entry.getAttribute('type') == 'desktop-id');
  if (launchables.isNotEmpty) {
    return launchables.any((entry) => _text(entry) == desktopId);
  }
  final id = _text(component.getElement('id'));
  return id != null && (id == desktopId || '$id.desktop' == desktopId);
}

String? _text(XmlElement? element) {
  final value = element?.innerText.replaceAll(RegExp(r'\s+'), ' ').trim();
  return value == null || value.isEmpty ? null : value;
}

String _normalizeLanguage(String language) => language
    .replaceFirst(RegExp(r'\.[^@]+'), '')
    .replaceAll('_', '-')
    .toLowerCase();

String? _language(XmlElement element) => element.getAttribute('xml:lang');

XmlElement? _localized(Iterable<XmlElement> variants, List<String> languages) {
  final translations = <String, XmlElement>{};
  XmlElement? original;
  for (final variant in variants) {
    final language = _language(variant);
    if (language == null) {
      original ??= variant;
    } else {
      translations.putIfAbsent(_normalizeLanguage(language), () => variant);
    }
  }
  for (final language in languages) {
    if (translations[language] case final match?) return match;
  }
  return original;
}

Iterable<XmlElement> _localizedBlocks(
  Iterable<XmlElement> elements,
  List<String> languages,
) sync* {
  final group = <XmlElement>[];
  for (final element in elements) {
    if (group.isNotEmpty &&
        (_language(element) == null ||
            element.name.local != group.first.name.local)) {
      if (_localized(group, languages) case final match?) yield match;
      group.clear();
    }
    group.add(element);
  }
  if (_localized(group, languages) case final match?) yield match;
}

String? _description(XmlElement? description, List<String> languages) {
  if (description == null) return null;
  final blocks = <String>[];
  for (final block in _localizedBlocks(description.childElements, languages)) {
    switch (block.name.local) {
      case 'p' || 'heading':
        if (_text(block) case final text?) blocks.add(text);
      case 'ul' || 'ol':
        final items = _localizedBlocks(
          block.findElements('li'),
          languages,
        ).map(_text).nonNulls.toList();
        if (items.isNotEmpty) blocks.add(items.join('\n'));
    }
  }
  if (blocks.isNotEmpty) return blocks.join('\n\n');
  return description.childElements.isEmpty ? _text(description) : null;
}
