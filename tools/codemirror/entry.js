// The hub's script editor: CodeMirror 6 with the Lua mode, bundled into one file
// (components/web/ui/cm.js) by build.sh. app.js uses window.HubEditor; if this file
// failed to load, it falls back to the plain textarea.
import { EditorView, keymap, lineNumbers, highlightActiveLine, highlightActiveLineGutter,
         drawSelection } from '@codemirror/view';
import { EditorState } from '@codemirror/state';
import { defaultKeymap, history, historyKeymap, indentWithTab } from '@codemirror/commands';
import { StreamLanguage, syntaxHighlighting, HighlightStyle, bracketMatching, indentOnInput,
         indentUnit } from '@codemirror/language';
import { search, searchKeymap, highlightSelectionMatches } from '@codemirror/search';
import { autocompletion, completionKeymap, closeBrackets, closeBracketsKeymap,
         completeFromList } from '@codemirror/autocomplete';
import { lua } from '@codemirror/legacy-modes/mode/lua';
import { tags as t } from '@lezer/highlight';

// Colours come from style.css (--tok-*), so the editor follows light/dark mode.
const hubHighlight = HighlightStyle.define([
  { tag: t.keyword, color: 'var(--tok-kw)' },
  { tag: [t.string, t.special(t.string)], color: 'var(--tok-str)' },
  { tag: [t.number, t.bool, t.null, t.atom], color: 'var(--tok-num)' },
  { tag: [t.lineComment, t.blockComment, t.comment], color: 'var(--tok-cmt)', fontStyle: 'italic' },
  { tag: [t.standard(t.variableName), t.function(t.variableName)], color: 'var(--tok-fn)' },
  { tag: t.operator, color: 'var(--muted)' },
]);

const theme = EditorView.theme({
  '&': { height: '100%', fontSize: '13px', backgroundColor: 'var(--bg)', color: 'var(--fg)' },
  '.cm-scroller': { fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace', lineHeight: '1.5' },
  '.cm-content': { caretColor: 'var(--fg)' },
  '.cm-gutters': { backgroundColor: 'var(--bg)', color: 'var(--muted)', borderRight: '1px solid var(--line)' },
  '.cm-activeLine, .cm-activeLineGutter': { backgroundColor: 'color-mix(in srgb, var(--accent) 8%, transparent)' },
  '&.cm-focused': { outline: 'none' },
  '&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection':
    { backgroundColor: 'color-mix(in srgb, var(--accent) 30%, transparent)' },
  '.cm-matchingBracket': { outline: '1px solid var(--muted)', backgroundColor: 'transparent' },
  '.cm-tooltip': { backgroundColor: 'var(--card)', color: 'var(--fg)', border: '1px solid var(--line)' },
  '.cm-tooltip-autocomplete > ul > li[aria-selected]': { backgroundColor: 'var(--accent)', color: 'var(--accent-fg)' },
  '.cm-panels': { backgroundColor: 'var(--card)', color: 'var(--fg)' },
});

// Completions: the hub's script API (keep in sync with the Template in app.js) + Lua.
const API = [
  ['zb.on(device, attr, fn)', 'call fn(value, device, attr) when it changes'],
  ['zb.get(device, attr)', 'current value (last known after a restart)'], ['zb.devices()', 'list of devices'],
  ['timer.after(s, fn)', 'once, s seconds from now'], ['timer.every(s, fn)', 'repeat every s seconds'],
  ['timer.cancel(id)', 'stop a timer'], ['cron("min hour day month weekday", fn)', 'local time'],
  ['state.get(key)', 'saved value'], ['state.put(key, value)', 'kept on flash'],
  ['secret(name)', 'value from Automations → Secrets'],
  ['http.get(url, opts)', 'never blocks; opts.on_done(status, body)'], ['http.post(url, body, opts)', ''],
  ['http.request{url=, method=}', ''], ['gpio.claim(pin, ...)', 'take pins at load (for handlers)'],
  ['gpio.write(pin, v)', ''], ['gpio.read(pin)', ''], ['gpio.on(pin, edge, fn)', '"rising" | "falling" | "both"'],
  ['gpio.beep(pin, ms)', 'active buzzer'], ['gpio.pattern(pins, steps, {rounds = n})', '{{mask, ms}, …}'],
  ['gpio.stop(pins)', ''], ['gpio.tone(pin, hz, ms)', 'passive buzzer'],
  ['led.set(r, g, b)', 'board RGB LED, 0..255 each'], ['led.off()', ''], ['led.claim()', 'take the LED at load (for handlers)'],
  ['net.online()', 'Wi-Fi up with an IP'], ['net.on("up" | "down", fn)', ''],
  ['json.encode(v)', ''], ['json.decode(s)', ''], ['log(...)', 'script log'],
].map(([label, detail]) => ({ label: label.replace(/\(.*$|\{.*$/, ''), detail: label.slice(label.search(/[({]/)) + (detail ? '  ' + detail : ''), type: 'function', apply: label.replace(/\(.+\)$/, '(').replace(/\{.+\}$/, '{') }));
const KEYWORDS = 'and break do else elseif end false for function goto if in local nil not or repeat return then true until while'
  .split(' ').map((k) => ({ label: k, type: 'keyword' }));

window.HubEditor = {
  // Creates an editor in `parent`; returns a small handle app.js uses.
  create(parent, doc, onChange) {
    const view = new EditorView({
      parent,
      state: EditorState.create({
        doc,
        extensions: [
          lineNumbers(), highlightActiveLineGutter(), highlightActiveLine(), drawSelection(),
          history(), indentOnInput(), bracketMatching(), closeBrackets(),
          indentUnit.of('  '), EditorState.tabSize.of(2),
          StreamLanguage.define(lua), syntaxHighlighting(hubHighlight),
          search({ top: true }), highlightSelectionMatches(),
          autocompletion({ override: [completeFromList([...API, ...KEYWORDS])] }),
          keymap.of([...closeBracketsKeymap, ...defaultKeymap, ...searchKeymap, ...historyKeymap,
                     ...completionKeymap, indentWithTab]),
          EditorView.contentAttributes.of({ autocapitalize: 'off', autocorrect: 'off', spellcheck: 'false' }),
          EditorView.updateListener.of((u) => { if (u.docChanged && onChange) onChange(); }),
          theme,
        ],
      }),
    });
    return {
      get: () => view.state.doc.toString(),
      set: (text) => view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: text } }),
      selectLine(n) {
        const line = view.state.doc.line(Math.max(1, Math.min(n, view.state.doc.lines)));
        view.dispatch({ selection: { anchor: line.from, head: line.to }, scrollIntoView: true });
        view.focus();
      },
      focus: () => view.focus(),
      destroy: () => view.destroy(),
    };
  },
};
