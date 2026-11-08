// The hub's script editor: CodeMirror 6 with the Lua mode, bundled into one file
// (components/web/ui/cm.js) by build.sh. app.js uses window.HubEditor; if this file
// failed to load, it falls back to the plain textarea.
import { EditorView, keymap, lineNumbers, highlightActiveLine, highlightActiveLineGutter,
         drawSelection } from '@codemirror/view';
import { EditorState, Compartment } from '@codemirror/state';
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

// Only what makes the editor sit in the page; everything else (cursor, selection, panels,
// tooltips, highlights) is CodeMirror's own light or dark default.
const THEME = {
  '&': { height: '100%', fontSize: '13px', backgroundColor: 'var(--bg)', color: 'var(--fg)' },
  '.cm-scroller': { fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace', lineHeight: '1.5' },
  '.cm-gutters': { backgroundColor: 'var(--bg)', color: 'var(--muted)', borderRight: '1px solid var(--line)' },
};

// CodeMirror picks its light or dark defaults by the theme's `dark` flag. The colours above
// follow the page by themselves, so the flag is kept in step with the page: the header
// toggle (data-theme) or the system.
const themeFor = (dark) => EditorView.theme(THEME, { dark });
const pageDark = () => {
  const t = document.documentElement.dataset.theme;
  return t ? t === 'dark' : matchMedia('(prefers-color-scheme: dark)').matches;
};

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
  ['motion.moving()', 'Wi-Fi motion now; nil = no signal'], ['motion.moved(from_s, to_s)', 'movement in that window (s ago); nil = not seen'],
  ['json.encode(v)', ''], ['json.decode(s)', ''], ['log(...)', 'script log'],
].map(([label, detail]) => ({ label: label.replace(/\(.*$|\{.*$/, ''), detail: label.slice(label.search(/[({]/)) + (detail ? '  ' + detail : ''), type: 'function', apply: label.replace(/\(.+\)$/, '(').replace(/\{.+\}$/, '{') }));
const KEYWORDS = 'and break do else elseif end false for function goto if in local nil not or repeat return then true until while'
  .split(' ').map((k) => ({ label: k, type: 'keyword' }));

window.HubEditor = {
  // Creates an editor in `parent`; returns a small handle app.js uses.
  create(parent, doc, onChange) {
    const mode = new Compartment();
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
          mode.of(themeFor(pageDark())),
        ],
      }),
    });
    const follow = () => view.dispatch({ effects: mode.reconfigure(themeFor(pageDark())) });
    const watch = new MutationObserver(follow);
    watch.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    const mq = matchMedia('(prefers-color-scheme: dark)');
    mq.addEventListener('change', follow);
    return {
      get: () => view.state.doc.toString(),
      set: (text) => view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: text } }),
      selectLine(n) {
        const line = view.state.doc.line(Math.max(1, Math.min(n, view.state.doc.lines)));
        view.dispatch({ selection: { anchor: line.from, head: line.to }, scrollIntoView: true });
        view.focus();
      },
      focus: () => view.focus(),
      destroy() { watch.disconnect(); mq.removeEventListener('change', follow); view.destroy(); },
    };
  },
};
