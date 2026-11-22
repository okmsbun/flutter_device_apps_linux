import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('flutter_device_apps_linux/methods');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const id = 'org.example.Editor.desktop';
  const info = {
    'desktopFileId': id,
    'appName': 'Editor',
    'supportsFiles': true,
    'supportsUris': false,
    'requiresTerminal': true,
  };

  setUp(() => debugDefaultTargetPlatformOverride = TargetPlatform.linux);
  tearDown(() {
    messenger.setMockMethodCallHandler(channel, null);
    debugDefaultTargetPlatformOverride = null;
  });

  test('search trims input and retains flags and relevance order', () async {
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'searchApps');
      expect(call.arguments, 'text editor');
      return [
        info,
        {'desktopFileId': 'Second.desktop', 'appName': 'Second'},
      ];
    });
    final results = await FlutterDeviceAppsLinux.searchApps(' text editor ');
    expect(results.map((app) => app.desktopFileId), [id, 'Second.desktop']);
    expect(results.first.supportsFiles, isTrue);
    expect(results.first.supportsUris, isFalse);
    expect(results.first.requiresTerminal, isTrue);
    expect(results.last.supportsFiles, isFalse);
    expect(await FlutterDeviceAppsLinux.searchApps('  '), isEmpty);
    await expectLater(
      FlutterDeviceAppsLinux.searchApps('a\u0000b'),
      throwsArgumentError,
    );
  });

  test('looks up MIME handlers and validates the media type', () async {
    messenger.setMockMethodCallHandler(channel, (call) async {
      if (call.arguments == 'text/plain') {
        return call.method == 'getDefaultAppForMimeType' ? info : [info];
      }
      return null;
    });
    expect(
      (await FlutterDeviceAppsLinux.getDefaultAppForMimeType(
        ' Text/Plain ',
      ))?.desktopFileId,
      id,
    );
    expect(
      (await FlutterDeviceAppsLinux.listAppsForMimeType(
        'text/plain',
      )).single.requiresTerminal,
      isTrue,
    );
    expect(
      await FlutterDeviceAppsLinux.getDefaultAppForMimeType(
        'application/example',
      ),
      isNull,
    );
    expect(
      await FlutterDeviceAppsLinux.listAppsForMimeType('application/example'),
      isEmpty,
    );
    for (final invalid in [
      '',
      'pdf',
      'text/*',
      'text/plain;charset=UTF-8',
      'text/\u0000plain',
    ]) {
      await expectLater(
        FlutterDeviceAppsLinux.listAppsForMimeType(invalid),
        throwsArgumentError,
      );
    }
  });

  group('AppStream metadata', () {
    late Directory temporary;
    late Directory metainfo;
    late List<String> roots;
    List<String> languages = const ['tr_TR.UTF-8', 'tr', 'C'];
    String? desktopPath;
    bool available = true;

    setUp(() {
      temporary = Directory.systemTemp.createTempSync('linux-metainfo-');
      metainfo = Directory('${temporary.path}/metainfo')..createSync();
      roots = [temporary.path];
      desktopPath = null;
      available = true;
      languages = const ['tr_TR.UTF-8', 'tr', 'C'];
      messenger.setMockMethodCallHandler(channel, (call) async {
        expect(call.method, 'getAppMetadata');
        expect(call.arguments, id);
        return available
            ? {
                'dataDirectories': roots,
                'languages': languages,
                'desktopFilePath': desktopPath,
              }
            : null;
      });
    });

    tearDown(() => temporary.deleteSync(recursive: true));

    test(
      'matches launchable ID before filename and uses localized text',
      () async {
        File(
          '${metainfo.path}/org.example.Editor.metainfo.xml',
        ).writeAsStringSync('''
        <component type="desktop-application">
          <id>org.example.Editor</id>
          <launchable type="desktop-id">Different.desktop</launchable>
          <developer><name>Wrong app</name></developer>
        </component>
      ''');
        File(
          '${metainfo.path}/org.example.Writer.metainfo.xml',
        ).writeAsStringSync('''
        <component type="desktop-application" xmlns="https://specifications.freedesktop.org/metainfo/1.0">
          <id>org.example.Writer</id>
          <launchable type="desktop-id">org.example.Editor.desktop</launchable>
          <url type="homepage">https://example.org/editor</url>
          <project_license>MIT OR Apache-2.0</project_license>
          <developer><name>Editor Team</name><name xml:lang="tr">Editör Ekibi</name></developer>
          <description>
            <p>Edit <em>text</em>.</p>
            <p xml:lang="tr">Metin düzenle.</p>
            <p>Second paragraph.</p>
            <ul><li>Open files.</li><li xml:lang="tr">Dosyaları aç.</li><li>Save files.</li></ul>
          </description>
        </component>
      ''');
        final metadata = (await FlutterDeviceAppsLinux.getAppMetadata(id))!;
        expect(metadata.homepage, Uri.parse('https://example.org/editor'));
        expect(metadata.developer, 'Editör Ekibi');
        expect(metadata.license, 'MIT OR Apache-2.0');
        expect(
          metadata.description,
          'Metin düzenle.\n\nSecond paragraph.\n\nDosyaları aç.\nSave files.',
        );
      },
    );

    test(
      'reads a legacy desktop id from appdata and leaves optional fields null',
      () async {
        final legacy = Directory('${temporary.path}/appdata')..createSync();
        File('${legacy.path}/$id.appdata.xml').writeAsStringSync('''
        <application>
          <id type="desktop">org.example.Editor.desktop</id>
          <developer_name>Legacy Team</developer_name>
          <url type="homepage">javascript:alert(1)</url>
          <description><p>Useful legacy app.</p></description>
        </application>
      ''');
        final metadata = (await FlutterDeviceAppsLinux.getAppMetadata(id))!;
        expect(metadata.homepage, isNull);
        expect(metadata.developer, 'Legacy Team');
        expect(metadata.license, isNull);
        expect(metadata.description, 'Useful legacy app.');
      },
    );

    test(
      'reads metainfo next to the target of an exported desktop symlink',
      () async {
        final appShare = Directory('${temporary.path}/flatpak/files/share');
        final apps = Directory('${appShare.path}/applications')
          ..createSync(recursive: true);
        final metadataDirectory = Directory('${appShare.path}/metainfo')
          ..createSync();
        final actualEntry = File('${apps.path}/$id')
          ..writeAsStringSync('[Desktop Entry]');
        final exported = Link('${temporary.path}/Export.desktop')
          ..createSync(actualEntry.path);
        desktopPath = exported.path;
        File(
          '${metadataDirectory.path}/org.example.Editor.metainfo.xml',
        ).writeAsStringSync('''
        <component type="desktop-application">
          <id>org.example.Editor</id>
          <launchable type="desktop-id">org.example.Editor.desktop</launchable>
          <project_license>MIT</project_license>
        </component>
      ''');
        expect(
          (await FlutterDeviceAppsLinux.getAppMetadata(id))?.license,
          'MIT',
        );
      },
    );

    test(
      'skips malformed and mismatched metadata and handles unavailable app',
      () async {
        File(
          '${metainfo.path}/org.example.Editor.metainfo.xml',
        ).writeAsStringSync('<component !');
        File('${metainfo.path}/Wrong.appdata.xml').writeAsStringSync('''
        <component type="desktop-application">
          <id>org.example.Editor</id>
          <launchable type="desktop-id">Different.desktop</launchable>
        </component>
      ''');
        expect(await FlutterDeviceAppsLinux.getAppMetadata(id), isNull);
        available = false;
        expect(await FlutterDeviceAppsLinux.getAppMetadata(id), isNull);
      },
    );
  });
}
