import 'package:flutter/services.dart';
import 'package:flutter_device_apps_linux_example/main.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:material_ui/material_ui.dart';

void main() {
  const channel = MethodChannel('flutter_device_apps_linux/methods');
  const id = 'org.example.Editor.desktop';
  const editor = {'desktopFileId': id, 'appName': 'Editor'};
  const browser = {
    'desktopFileId': 'org.example.Browser.desktop',
    'appName': 'Browser',
  };
  final calls = <MethodCall>[];
  const linux = TargetPlatformVariant({TargetPlatform.linux});

  setUp(() {
    calls.clear();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
          calls.add(call);
          return switch (call.method) {
            'listApps' => [editor, browser],
            'searchApps' => [editor],
            'getApp' => editor,
            'getAppIcon' || 'getAppMetadata' => null,
            'getInstallSourceInfo' => {'source': 'unknown'},
            'detectAppTechnologies' => ['libQt6Core.so.6'],
            'getDefaultAppForMimeType' || 'getDefaultAppForFile' => editor,
            'listAppsForMimeType' || 'listAppsForFile' => [editor, browser],
            'openApp' => true,
            _ => throw MissingPluginException(call.method),
          };
        });
  });

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  testWidgets('searches apps and opens the selected desktop entry', (
    tester,
  ) async {
    await tester.pumpWidget(const LinuxDeviceAppsExample());
    await tester.pumpAndSettle();
    expect(find.text('Browser'), findsOneWidget);

    await tester.enterText(find.byType(TextField), 'editor');
    await tester.tap(find.byTooltip('Search'));
    await tester.pumpAndSettle();
    expect(calls.last.method, 'searchApps');
    expect(calls.last.arguments, 'editor');
    expect(find.text('Browser'), findsNothing);

    await tester.tap(find.text('Editor'));
    await tester.pumpAndSettle();
    expect(find.text('Desktop ID'), findsOneWidget);
    expect(find.text(id), findsOneWidget);

    await tester.tap(find.text('Open app'));
    await tester.pumpAndSettle();
    expect(calls.last.method, 'openApp');
    expect(calls.last.arguments, id);
    expect(find.text('Launch requested.'), findsOneWidget);
  }, variant: linux);

  testWidgets('shows the default handler once and lists alternatives', (
    tester,
  ) async {
    await tester.pumpWidget(const LinuxDeviceAppsExample());
    await tester.pumpAndSettle();
    await tester.tap(find.byTooltip('Find handlers'));
    await tester.pumpAndSettle();

    await tester.enterText(find.byType(TextField), 'text/plain');
    await tester.tap(find.byTooltip('Look up handlers'));
    await tester.pumpAndSettle();
    expect(calls.map((call) => call.method), contains('listAppsForMimeType'));
    expect(find.text('Default'), findsOneWidget);
    expect(find.text('Editor'), findsOneWidget);
    expect(find.text('Browser'), findsOneWidget);

    await tester.enterText(find.byType(TextField), '/tmp/report.txt');
    await tester.tap(find.byTooltip('Look up handlers'));
    await tester.pumpAndSettle();
    expect(calls.map((call) => call.method), contains('listAppsForFile'));
  }, variant: linux);
}
