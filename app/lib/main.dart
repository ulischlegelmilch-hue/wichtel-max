import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  runApp(const WichtelApp());
}

// ---------------------------------------------------------------------------
// Backend-Zugriff (dieselbe REST-API wie die Web-App)
// ---------------------------------------------------------------------------
class Api {
  static String baseUrl = '';

  static Future<void> load() async {
    final p = await SharedPreferences.getInstance();
    baseUrl = p.getString('baseUrl') ?? '';
  }

  static Future<void> setUrl(String u) async {
    baseUrl = u.trim().replaceAll(RegExp(r'/+$'), '');
    final p = await SharedPreferences.getInstance();
    await p.setString('baseUrl', baseUrl);
  }

  static bool get configured => baseUrl.isNotEmpty;
  static Uri _u(String path) => Uri.parse('$baseUrl$path');
  static const _timeout = Duration(seconds: 8);

  static Future<Map<String, dynamic>> state() async {
    final r = await http.get(_u('/api/state')).timeout(_timeout);
    if (r.statusCode != 200) throw 'HTTP ${r.statusCode}';
    return jsonDecode(r.body) as Map<String, dynamic>;
  }

  static Future<bool> post(String path, Map<String, dynamic> body) async {
    final r = await http
        .post(_u(path),
            headers: {'Content-Type': 'application/json'},
            body: jsonEncode(body))
        .timeout(_timeout);
    return r.statusCode == 200;
  }

  static Future<bool> del(String path) async {
    final r = await http.delete(_u(path)).timeout(_timeout);
    return r.statusCode == 200;
  }
}

// ---------------------------------------------------------------------------
class WichtelApp extends StatelessWidget {
  const WichtelApp({super.key});

  @override
  Widget build(BuildContext context) {
    final scheme = ColorScheme.fromSeed(
      seedColor: const Color(0xFF1F7A3D),
      brightness: Brightness.dark,
    );
    return MaterialApp(
      title: 'Wichtel Max',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: scheme,
        scaffoldBackgroundColor: const Color(0xFF0E1A12),
        inputDecorationTheme: const InputDecorationTheme(
          border: OutlineInputBorder(),
          isDense: true,
        ),
      ),
      home: const HomePage(),
    );
  }
}

// ---------------------------------------------------------------------------
class HomePage extends StatefulWidget {
  const HomePage({super.key});
  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  int _tab = 0;
  Map<String, dynamic>? _state;
  String? _error;
  bool _loading = true;
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    _init();
  }

  Future<void> _init() async {
    await Api.load();
    if (!Api.configured) {
      setState(() => _loading = false);
      WidgetsBinding.instance.addPostFrameCallback((_) => _openSettings());
    } else {
      _refresh();
    }
    _timer = Timer.periodic(const Duration(seconds: 5), (_) => _refresh(silent: true));
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  Future<void> _refresh({bool silent = false}) async {
    if (!Api.configured) return;
    if (!silent) setState(() => _loading = true);
    try {
      final s = await Api.state();
      if (mounted) setState(() { _state = s; _error = null; _loading = false; });
    } catch (e) {
      if (mounted) setState(() { _error = '$e'; _loading = false; });
    }
  }

  void _toast(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context)
      ..clearSnackBars()
      ..showSnackBar(SnackBar(content: Text(msg)));
  }

  Future<void> _openSettings() async {
    await showModalBottomSheet(
      context: context,
      isScrollControlled: true,
      showDragHandle: true,
      builder: (_) => SettingsSheet(state: _state),
    );
    _refresh();
  }

  @override
  Widget build(BuildContext context) {
    final online = _state?['deviceOnline'] == true;
    final batt = _state?['deviceBatt'];
    final stars = _state?['stars'] ?? 0;

    return Scaffold(
      appBar: AppBar(
        title: const Text('🎄 Wichtel Max'),
        actions: [
          if (_state != null)
            Padding(
              padding: const EdgeInsets.only(right: 4),
              child: Center(
                child: Row(children: [
                  Icon(Icons.circle, size: 11,
                      color: online ? Colors.greenAccent : Colors.grey),
                  const SizedBox(width: 4),
                  if (batt is int && batt >= 0)
                    Text(batt < 15 ? '⚠️$batt% ' : '🔋$batt% ',
                        style: TextStyle(
                            fontSize: 13,
                            color: batt < 15 ? Colors.orangeAccent : null,
                            fontWeight: batt < 15 ? FontWeight.bold : null)),
                ]),
              ),
            ),
          IconButton(
            icon: const Icon(Icons.settings),
            onPressed: _openSettings,
          ),
        ],
      ),
      body: !Api.configured
          ? _needsSetup()
          : RefreshIndicator(
              onRefresh: _refresh,
              child: _body(stars),
            ),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _tab,
        onDestinationSelected: (i) => setState(() => _tab = i),
        destinations: const [
          NavigationDestination(icon: Icon(Icons.mail_outline), label: 'Nachricht'),
          NavigationDestination(icon: Icon(Icons.check_circle_outline), label: 'Aufgaben'),
          NavigationDestination(icon: Icon(Icons.chat_bubble_outline), label: 'Max'),
        ],
      ),
    );
  }

  Widget _needsSetup() => Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              const Icon(Icons.wifi_tethering, size: 48),
              const SizedBox(height: 12),
              const Text('Backend-Adresse fehlt',
                  style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold)),
              const SizedBox(height: 8),
              const Text(
                'Trage die Adresse deines Wichtel-Backends ein '
                '(z. B. http://192.168.1.50:8080 oder deine Cloud-Adresse).',
                textAlign: TextAlign.center,
              ),
              const SizedBox(height: 16),
              FilledButton.icon(
                onPressed: _openSettings,
                icon: const Icon(Icons.settings),
                label: const Text('Einrichten'),
              ),
            ],
          ),
        ),
      );

  Widget _body(int stars) {
    if (_loading && _state == null) {
      return const Center(child: CircularProgressIndicator());
    }
    if (_error != null && _state == null) {
      return ListView(children: [
        const SizedBox(height: 80),
        Center(child: Text('Backend nicht erreichbar\n$_error',
            textAlign: TextAlign.center)),
        const SizedBox(height: 16),
        Center(child: FilledButton(onPressed: _refresh, child: const Text('Nochmal'))),
      ]);
    }
    final s = _state ?? {};
    switch (_tab) {
      case 0:
        return SendTab(state: s, toast: _toast, refresh: _refresh);
      case 1:
        return TasksTab(state: s, stars: stars, toast: _toast, refresh: _refresh);
      default:
        return MaxTab(state: s);
    }
  }
}

// ---------------------------------------------------------------------------
// Tab 1: Nachricht senden
// ---------------------------------------------------------------------------
class SendTab extends StatefulWidget {
  final Map<String, dynamic> state;
  final void Function(String) toast;
  final Future<void> Function() refresh;
  const SendTab({super.key, required this.state, required this.toast, required this.refresh});
  @override
  State<SendTab> createState() => _SendTabState();
}

class _SendTabState extends State<SendTab> {
  final _text = TextEditingController();
  final _from = TextEditingController(text: 'Lumbi');
  bool _sending = false;

  static const _presets = [
    'Guten Morgen, Max! 🌟',
    'Ich hab dich lieb!',
    'Räum bitte dein Zimmer auf 😉',
    'Heute gibt\'s eine Überraschung 🎁',
    'Putz die Zähne 🦷',
    'Gute Nacht, schlaf schön! 🌙',
  ];

  @override
  void dispose() {
    _text.dispose();
    _from.dispose();
    super.dispose();
  }

  Future<void> _send() async {
    final t = _text.text.trim();
    if (t.isEmpty) { widget.toast('Bitte eine Nachricht eintippen'); return; }
    setState(() => _sending = true);
    final ok = await Api.post('/api/message', {'text': t, 'from': _from.text.trim().isEmpty ? 'Lumbi' : _from.text.trim()});
    setState(() => _sending = false);
    if (ok) { _text.clear(); widget.toast('🎁 Gesendet!'); widget.refresh(); }
    else { widget.toast('Fehler beim Senden'); }
  }

  @override
  Widget build(BuildContext context) {
    final last = widget.state['last'];
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        const Text('Nachricht von Lumbi', style: TextStyle(fontWeight: FontWeight.bold)),
        const SizedBox(height: 8),
        TextField(
          controller: _text,
          minLines: 3, maxLines: 6,
          decoration: const InputDecoration(hintText: 'Hallo Max! …'),
        ),
        const SizedBox(height: 8),
        Wrap(spacing: 6, runSpacing: 6, children: [
          for (final p in _presets)
            ActionChip(label: Text(p), onPressed: () => setState(() => _text.text = p)),
        ]),
        const SizedBox(height: 12),
        TextField(controller: _from, decoration: const InputDecoration(labelText: 'Absender')),
        const SizedBox(height: 16),
        FilledButton.icon(
          onPressed: _sending ? null : _send,
          icon: const Icon(Icons.send),
          label: Text(_sending ? 'Sende …' : 'An den Wichtel senden'),
        ),
        const SizedBox(height: 24),
        if (last != null) Card(
          child: ListTile(
            title: Text(last['text'] ?? ''),
            subtitle: Text('zuletzt gesendet · ${last['from'] ?? ''}'),
            leading: const Icon(Icons.history),
          ),
        ),
      ],
    );
  }
}

// ---------------------------------------------------------------------------
// Tab 2: Aufgaben
// ---------------------------------------------------------------------------
class TasksTab extends StatefulWidget {
  final Map<String, dynamic> state;
  final int stars;
  final void Function(String) toast;
  final Future<void> Function() refresh;
  const TasksTab({super.key, required this.state, required this.stars, required this.toast, required this.refresh});
  @override
  State<TasksTab> createState() => _TasksTabState();
}

class _TasksTabState extends State<TasksTab> {
  final _text = TextEditingController();
  String _scope = 'once';
  bool _busy = false;

  static const _scopeLabel = {
    'once': 'einmalig', 'day': 'jeden Tag', 'week': 'jede Woche', 'month': 'jeden Monat',
  };

  @override
  void dispose() { _text.dispose(); super.dispose(); }

  Future<void> _add() async {
    final t = _text.text.trim();
    if (t.isEmpty) { widget.toast('Bitte eine Aufgabe eintippen'); return; }
    setState(() => _busy = true);
    final ok = await Api.post('/api/task', {'text': t, 'scope': _scope, 'from': 'Lumbi'});
    setState(() => _busy = false);
    if (ok) { _text.clear(); widget.toast('✅ Aufgabe hinzugefügt'); widget.refresh(); }
    else { widget.toast('Fehler'); }
  }

  Future<void> _done(int id) async {
    if (await Api.post('/api/task/$id/done', {})) { widget.toast('✓ Erledigt'); widget.refresh(); }
  }

  Future<void> _delete(int id) async {
    if (await Api.del('/api/task/$id')) { widget.toast('Gelöscht'); widget.refresh(); }
  }

  @override
  Widget build(BuildContext context) {
    final tasks = (widget.state['tasks'] as List?) ?? [];
    final open = tasks.where((t) => t['done'] != true).toList();
    final done = tasks.where((t) => t['done'] == true).toList();
    final sorted = [...open, ...done];

    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
          const Text('Aufgaben für Max', style: TextStyle(fontWeight: FontWeight.bold)),
          Text('⭐ ${widget.stars}', style: const TextStyle(color: Color(0xFFFFD479), fontWeight: FontWeight.bold, fontSize: 16)),
        ]),
        const SizedBox(height: 8),
        TextField(controller: _text, decoration: const InputDecoration(hintText: 'z. B. Zähne putzen')),
        const SizedBox(height: 8),
        Row(children: [
          Expanded(
            child: DropdownButtonFormField<String>(
              initialValue: _scope,
              decoration: const InputDecoration(labelText: 'Zeitraum'),
              items: [
                for (final e in _scopeLabel.entries)
                  DropdownMenuItem(value: e.key, child: Text(e.value)),
              ],
              onChanged: (v) => setState(() => _scope = v ?? 'once'),
            ),
          ),
          const SizedBox(width: 8),
          FilledButton(onPressed: _busy ? null : _add, child: const Icon(Icons.add)),
        ]),
        const SizedBox(height: 16),
        if (sorted.isEmpty) const Padding(
          padding: EdgeInsets.symmetric(vertical: 24),
          child: Center(child: Text('Noch keine Aufgaben.')),
        ),
        for (final t in sorted) Card(
          child: ListTile(
            leading: Icon(t['done'] == true ? Icons.check_circle : Icons.circle_outlined,
                color: t['done'] == true ? Colors.greenAccent : null),
            title: Text(t['text'] ?? '',
                style: TextStyle(
                    decoration: t['done'] == true ? TextDecoration.lineThrough : null)),
            subtitle: Text('${_scopeLabel[t['scope']] ?? 'einmalig'} · ${t['done'] == true ? 'erledigt' : 'offen'}'),
            trailing: Row(mainAxisSize: MainAxisSize.min, children: [
              if (t['done'] != true)
                IconButton(icon: const Icon(Icons.check), tooltip: 'erledigt',
                    onPressed: () => _done(t['id'])),
              IconButton(icon: const Icon(Icons.delete_outline), tooltip: 'löschen',
                  onPressed: () => _delete(t['id'])),
            ]),
          ),
        ),
      ],
    );
  }
}

// ---------------------------------------------------------------------------
// Tab 3: Max (Antworten + Status)
// ---------------------------------------------------------------------------
class MaxTab extends StatelessWidget {
  final Map<String, dynamic> state;
  const MaxTab({super.key, required this.state});

  @override
  Widget build(BuildContext context) {
    final replies = (state['replies'] as List?) ?? [];
    final online = state['deviceOnline'] == true;
    final batt = state['deviceBatt'];
    final fw = state['deviceFw'];
    final ota = state['ota'] as Map<String, dynamic>?;

    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        Card(
          child: Padding(
            padding: const EdgeInsets.all(14),
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              const Text('Gerät', style: TextStyle(fontWeight: FontWeight.bold)),
              const SizedBox(height: 8),
              Row(children: [
                Icon(Icons.circle, size: 12, color: online ? Colors.greenAccent : Colors.grey),
                const SizedBox(width: 6),
                Text(online ? 'online' : 'offline'),
                const Spacer(),
                if (batt is int && batt >= 0)
                  Text(batt < 15 ? '⚠️ Akku fast leer: $batt%' : '🔋 $batt%',
                      style: TextStyle(
                          color: batt < 15 ? Colors.orangeAccent : null,
                          fontWeight: batt < 15 ? FontWeight.bold : null)),
              ]),
              if (fw != null) Padding(
                padding: const EdgeInsets.only(top: 6),
                child: Text('Firmware v$fw'
                    '${ota != null && ota['hasBin'] == true && (ota['version'] ?? 0) > (fw ?? 0) ? '  ·  Update v${ota['version']} bereit' : ''}',
                    style: const TextStyle(fontSize: 13, color: Colors.white70)),
              ),
            ]),
          ),
        ),
        const SizedBox(height: 12),
        const Text('💬 Max hat geantwortet', style: TextStyle(fontWeight: FontWeight.bold)),
        const SizedBox(height: 8),
        if (replies.isEmpty) const Padding(
          padding: EdgeInsets.symmetric(vertical: 16),
          child: Text('Noch keine Antworten von Max.'),
        ),
        for (final r in replies) Card(
          child: ListTile(
            leading: const Text('💌', style: TextStyle(fontSize: 22)),
            title: Text(r['text'] ?? ''),
            subtitle: Text(_ts(r['ts'])),
          ),
        ),
      ],
    );
  }

  static String _ts(dynamic ms) {
    if (ms is! int) { return ''; }
    final d = DateTime.fromMillisecondsSinceEpoch(ms);
    String two(int n) => n.toString().padLeft(2, '0');
    return '${two(d.day)}.${two(d.month)}. ${two(d.hour)}:${two(d.minute)}';
  }
}

// ---------------------------------------------------------------------------
// Einstellungen (Backend-Adresse + Fern-Konfiguration)
// ---------------------------------------------------------------------------
class SettingsSheet extends StatefulWidget {
  final Map<String, dynamic>? state;
  const SettingsSheet({super.key, this.state});
  @override
  State<SettingsSheet> createState() => _SettingsSheetState();
}

class _SettingsSheetState extends State<SettingsSheet> {
  late final TextEditingController _url = TextEditingController(text: Api.baseUrl);
  final _cmd = TextEditingController();

  @override
  void dispose() { _url.dispose(); _cmd.dispose(); super.dispose(); }

  void _toast(String m) => ScaffoldMessenger.of(context)
      .showSnackBar(SnackBar(content: Text(m)));

  @override
  Widget build(BuildContext context) {
    final cfg = widget.state?['config'] as Map<String, dynamic>?;
    return Padding(
      padding: EdgeInsets.only(
        left: 16, right: 16, top: 8,
        bottom: MediaQuery.of(context).viewInsets.bottom + 16,
      ),
      child: ListView(
        shrinkWrap: true,
        children: [
          const Text('Einstellungen', style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold)),
          const SizedBox(height: 12),
          TextField(
            controller: _url,
            keyboardType: TextInputType.url,
            decoration: const InputDecoration(
              labelText: 'Backend-Adresse',
              hintText: 'https://wichtel-max.onrender.com  (oder http://192.168.1.50:8080)',
            ),
          ),
          const SizedBox(height: 8),
          FilledButton(
            onPressed: () async {
              await Api.setUrl(_url.text);
              if (!context.mounted) return;
              _toast('Gespeichert');
              Navigator.pop(context);
            },
            child: const Text('Speichern'),
          ),
          const Divider(height: 32),
          const Text('Fernsteuerung', style: TextStyle(fontWeight: FontWeight.bold)),
          const SizedBox(height: 8),
          Row(children: [
            Expanded(child: TextField(controller: _cmd,
                decoration: const InputDecoration(hintText: 'Befehl, z. B. beep 3'))),
            const SizedBox(width: 8),
            FilledButton(
              onPressed: () async {
                final c = _cmd.text.trim();
                if (c.isEmpty) return;
                final ok = await Api.post('/api/cmd-queue', {'cmd': c});
                if (!mounted) return;
                if (ok) { _cmd.clear(); _toast('An das Gerät geschickt'); }
                else { _toast('Fehler'); }
              },
              child: const Text('senden'),
            ),
          ]),
          if (cfg != null) Padding(
            padding: const EdgeInsets.only(top: 12),
            child: Text(
              'Aktuell: Poll ${cfg['pollMin']} min · Nacht ${cfg['nightStart']}–${cfg['nightEnd']} Uhr · Vol ${cfg['volume']}',
              style: const TextStyle(fontSize: 12, color: Colors.white70),
            ),
          ),
        ],
      ),
    );
  }
}
