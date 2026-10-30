(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const exprEl = $('expr');
  const previewEl = $('preview');
  const previousEl = $('previous');
  const statusEl = $('status');
  const displayEl = $('display');
  const historyEl = $('history-list');
  const angleKey = $('angle-key');

  const ANGLES = ['deg', 'rad', 'grad'];
  const OPERATOR_START = /^(\s*mod\b|[+\-−×÷*/^!%°),])/;
  const HISTORY_LIMIT = 50;
  const coarsePointer = window.matchMedia('(pointer: coarse)').matches;

  // localStorage can be unavailable (private mode, blocked storage); the app works without it.
  const store = {
    get(key, fallback) {
      try {
        const v = localStorage.getItem(key);
        return v === null ? fallback : JSON.parse(v);
      } catch {
        return fallback;
      }
    },
    set(key, value) {
      try { localStorage.setItem(key, JSON.stringify(value)); } catch { /* ignore */ }
    },
  };

  const state = {
    angle: ANGLES.includes(store.get('angle')) ? store.get('angle') : 'deg',
    second: false,
    hyp: false,
    ans: store.get('ans', '0'),
    mem: store.get('mem', '0'),
    history: store.get('history', []),
    justEvaluated: false,
  };

  // ------------------------------------------------------------- backend

  async function evaluate(expr) {
    const params = new URLSearchParams({ expr, angle: state.angle, ans: state.ans, mem: state.mem });
    try {
      const res = await fetch(`/api/eval?${params}`);
      const data = await res.json();
      setOnline(true);
      return data;
    } catch {
      setOnline(false);
      return { ok: false, error: 'Backend unreachable — is the C server running?' };
    }
  }

  function setOnline(online) {
    statusEl.classList.toggle('online', online);
    statusEl.classList.toggle('offline', !online);
    statusEl.textContent = online ? 'C backend online' : 'backend offline';
  }

  // ------------------------------------------------------------ rendering

  function trigName(base) {
    return (state.second ? 'a' : '') + base + (state.hyp ? 'h' : '');
  }

  function renderIndicators() {
    const unit = state.angle.toUpperCase();
    angleKey.textContent = unit;
    $('ind-angle').textContent = unit;
    $('ind-2nd').classList.toggle('on', state.second);
    $('ind-hyp').classList.toggle('on', state.hyp);
    $('ind-mem').classList.toggle('on', Number(state.mem) !== 0);
    document.querySelector('[data-action="second"]').classList.toggle('on', state.second);
    document.querySelector('[data-action="hyp"]').classList.toggle('on', state.hyp);
    for (const btn of document.querySelectorAll('[data-trig]')) {
      const base = btn.dataset.trig + (state.hyp ? 'h' : '');
      btn.innerHTML = state.second ? `${base}<sup>−1</sup>` : base;
    }
  }

  function renderHistory() {
    historyEl.replaceChildren(...state.history.map((item) => {
      const li = document.createElement('li');
      const btn = document.createElement('button');
      const expr = document.createElement('span');
      const result = document.createElement('span');
      btn.type = 'button';
      btn.className = 'history-item';
      btn.title = 'Load this expression';
      expr.className = 'h-expr';
      expr.textContent = `${item.expr} =`;
      result.className = 'h-result';
      result.textContent = item.result;
      btn.append(expr, result);
      btn.addEventListener('click', () => {
        setExpr(item.expr);
        state.justEvaluated = false;
        schedulePreview();
        focusInput();
      });
      li.append(btn);
      return li;
    }));
    $('history-empty').hidden = state.history.length > 0;
  }

  function showPreview(text) {
    previewEl.textContent = text;
    previewEl.classList.remove('error');
  }

  function showError(message) {
    previewEl.textContent = message;
    previewEl.classList.add('error');
    displayEl.classList.remove('shake');
    void displayEl.offsetWidth; // restart the animation
    displayEl.classList.add('shake');
  }

  // --------------------------------------------------------------- editing

  function focusInput() {
    if (!coarsePointer) exprEl.focus();
  }

  function setExpr(text) {
    exprEl.value = text;
    exprEl.setSelectionRange(text.length, text.length);
  }

  function insert(text) {
    if (state.justEvaluated) {
      // An operator continues from the result; anything else starts a new expression.
      if (!OPERATOR_START.test(text)) setExpr('');
      state.justEvaluated = false;
    }
    const value = exprEl.value;
    const start = exprEl.selectionStart ?? value.length;
    const end = exprEl.selectionEnd ?? start;
    exprEl.value = value.slice(0, start) + text + value.slice(end);
    const caret = start + text.length;
    exprEl.setSelectionRange(caret, caret);
    schedulePreview();
  }

  function backspace() {
    const value = exprEl.value;
    const start = exprEl.selectionStart ?? value.length;
    const end = exprEl.selectionEnd ?? start;
    state.justEvaluated = false;
    if (start !== end) {
      exprEl.value = value.slice(0, start) + value.slice(end);
      exprEl.setSelectionRange(start, start);
    } else if (start > 0) {
      exprEl.value = value.slice(0, start - 1) + value.slice(start);
      exprEl.setSelectionRange(start - 1, start - 1);
    }
    schedulePreview();
  }

  function isBalanced(text) {
    let depth = 0;
    for (const ch of text) {
      if (ch === '(') depth++;
      else if (ch === ')' && --depth < 0) return false;
    }
    return depth === 0;
  }

  function negate() {
    const value = exprEl.value.trim();
    if (!value) return;
    const wrapped = value.match(/^−\((.*)\)$/);
    setExpr(wrapped && isBalanced(wrapped[1]) ? wrapped[1] : `−(${value})`);
    state.justEvaluated = false;
    schedulePreview();
  }

  // ------------------------------------------------------------- evaluation

  let previewTimer = 0;
  let previewSeq = 0;

  function schedulePreview() {
    clearTimeout(previewTimer);
    previewTimer = setTimeout(updatePreview, 120);
  }

  async function updatePreview() {
    const expr = exprEl.value.trim();
    const seq = ++previewSeq;
    if (!expr || state.justEvaluated) {
      showPreview('');
      return;
    }
    const data = await evaluate(expr);
    if (seq !== previewSeq) return; // a newer keystroke superseded this request
    showPreview(data.ok && data.result !== expr ? `= ${data.result}` : '');
  }

  async function equals() {
    const expr = exprEl.value.trim();
    if (!expr) return;
    previewSeq++;
    const data = await evaluate(expr);
    if (!data.ok) {
      showError(data.error);
      return;
    }
    state.history.unshift({ expr, result: data.result });
    state.history.length = Math.min(state.history.length, HISTORY_LIMIT);
    state.ans = data.raw;
    store.set('history', state.history);
    store.set('ans', state.ans);
    renderHistory();

    previousEl.textContent = `${expr} =`;
    setExpr(data.result);
    showPreview('');
    state.justEvaluated = true;
  }

  async function memoryOp(sign) {
    const expr = exprEl.value.trim() || 'ans';
    const data = await evaluate(`M${sign}(${expr})`);
    if (!data.ok) {
      showError(data.error);
      return;
    }
    state.mem = data.raw;
    store.set('mem', state.mem);
    renderIndicators();
    showPreview(`M = ${data.result}`);
    state.justEvaluated = true;
  }

  const actions = {
    equals,
    negate,
    back: backspace,
    clear() {
      setExpr('');
      previousEl.textContent = '';
      showPreview('');
      state.justEvaluated = false;
    },
    second() {
      state.second = !state.second;
      renderIndicators();
    },
    hyp() {
      state.hyp = !state.hyp;
      renderIndicators();
    },
    angle() {
      state.angle = ANGLES[(ANGLES.indexOf(state.angle) + 1) % ANGLES.length];
      store.set('angle', state.angle);
      renderIndicators();
      schedulePreview();
    },
    mc() {
      state.mem = '0';
      store.set('mem', state.mem);
      renderIndicators();
      showPreview('Memory cleared');
    },
    mr: () => insert('M'),
    mplus: () => memoryOp('+'),
    mminus: () => memoryOp('−'),
  };

  // ---------------------------------------------------------------- events

  document.querySelector('.keys').addEventListener('click', (e) => {
    const btn = e.target.closest('button');
    if (!btn) return;
    if (btn.dataset.trig) {
      insert(`${trigName(btn.dataset.trig)}(`);
      state.second = false; // 2nd and hyp apply to one keypress, like a hardware calculator
      state.hyp = false;
      renderIndicators();
    } else if (btn.dataset.insert !== undefined) {
      insert(btn.dataset.insert);
    } else if (btn.dataset.action) {
      actions[btn.dataset.action]();
    }
    focusInput();
  });

  exprEl.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' || e.key === '=') {
      e.preventDefault();
      equals();
    } else if (e.key === 'Escape') {
      e.preventDefault();
      actions.clear();
    } else if (state.justEvaluated && e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey) {
      if (!OPERATOR_START.test(e.key)) setExpr('');
      state.justEvaluated = false;
    }
  });

  exprEl.addEventListener('input', () => {
    state.justEvaluated = false;
    schedulePreview();
  });

  // Typing anywhere on the page goes to the expression field.
  document.addEventListener('keydown', (e) => {
    if (e.target === exprEl || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.key.length === 1 || e.key === 'Backspace' || e.key === 'Enter' || e.key === 'Escape') {
      if (e.key === 'Enter' && e.target.closest?.('button')) return;
      exprEl.focus();
    }
  });

  $('clear-history').addEventListener('click', () => {
    state.history = [];
    store.set('history', state.history);
    renderHistory();
  });

  // ------------------------------------------------------------------ init

  renderIndicators();
  renderHistory();
  focusInput();
  fetch('/api/health')
    .then((res) => res.json())
    .then((data) => setOnline(Boolean(data.ok)))
    .catch(() => setOnline(false));
})();
