'use strict';
(() => {
  const byId = id => document.getElementById(id);
  const graph = byId('graph'), dialog = byId('trace-dialog');
  let instance = null, observationVersion = null, renderedVersion = null;
  let fact = null, cursor = null, records = [];
  let supportGeneration = 0, supportBusy = false, supportPending = false, supportAbort;
  let graphGeneration = 0, graphAbort;
  let selection = null;
  const compactMap = matchMedia('(max-width: 760px)');

  function revealTopology() {
    const first = graph.querySelector('[role=button]');
    if (!compactMap.matches || !first) return;
    const bounds = graph.getBoundingClientRect(), target = first.getBoundingClientRect();
    graph.scrollTo({left:Math.max(0, graph.scrollLeft + target.left - bounds.left - 24),
      top:Math.max(0, graph.scrollTop + target.top - bounds.top - 24)});
  }
  compactMap.addEventListener('change', event => { if (event.matches) revealTopology(); });

  const keyOf = value => `${value.traceId}/${value.incarnation}`;
  const countText = (count, noun) => `${count} ${noun}${count === 1 ? "" : "s"}`;
  const age = millis => {
    const seconds = Math.floor(Number(millis) / 1000);
    return seconds < 60 ? `${seconds}s ago` : seconds < 3600
      ? `${Math.floor(seconds / 60)}m ago` : `${Math.floor(seconds / 3600)}h ago`;
  };
  const text = (tag, value, className) => {
    const element = document.createElement(tag);
    element.textContent = value;
    if (className) element.className = className;
    return element;
  };

  function freezeSelection(message) {
    if (!selection || selection.stale) return;
    selection.stale = true;
    selection.abort?.abort();
    byId('trace-status').textContent = message;
    byId('trace-status').classList.add('stale');
    byId('trace-retry').hidden = true;
  }

  function acceptInstance(next) {
    if (instance === next) return false;
    const previous = instance;
    instance = next;
    observationVersion = null;
    renderedVersion = null;
    if (previous !== null) {
      freezeSelection('No longer retained in this observation instance. Showing the last snapshot.');
      ++supportGeneration;
      supportAbort?.abort(); supportBusy = false; supportPending = false;
      ++graphGeneration; graphAbort?.abort(); graph.replaceChildren();
      records = []; cursor = null; renderRecords();
    }
    return true;
  }

  function renderRecords() {
    const list = byId('traces');
    const existing = new Map([...list.children].map(item => [item.firstElementChild.dataset.recordKey, item]));
    const focusedKey = document.activeElement?.closest('#traces button')?.dataset.recordKey;
    records.forEach((record, index) => {
      const key = keyOf(record);
      let item = existing.get(key);
      if (!item) {
        item = document.createElement('li');
        const button = document.createElement('button');
        button.className = 'trace-row'; button.dataset.recordKey = key;
        button.addEventListener('click', () => {
          const current = records.find(value => keyOf(value) === key);
          if (current) openTrace(current);
        });
        item.append(button);
      }
      existing.delete(key);
      const row = item.firstElementChild;
      row.setAttribute('aria-current', String(Boolean(selection && !selection.stale
        && selection.instance === instance && keyOf(selection) === key)));
      row.replaceChildren(text('code', `${record.traceId.slice(0, 8)}…${record.traceId.slice(-8)}`),
        text('small', `${record.lifecycle} · ${countText(record.spanCount, "span")} · ${countText(record.serviceCount, "service")} · ${age(record.evidenceAgeMillis)}`));
      if (list.children[index] !== item) list.insertBefore(item, list.children[index] ?? null);
    });
    for (const item of existing.values()) item.remove();
    if (focusedKey && !dialog.open) {
      [...list.querySelectorAll('button')].find(row => row.dataset.recordKey === focusedKey)?.focus({preventScroll:true});
    }
    byId('more').hidden = !cursor;
    byId('more').disabled = supportBusy;
  }

  async function refreshSupport(reset = true) {
    if (!fact || !instance) return;
    if (supportBusy) { supportPending = true; return; }
    supportBusy = true; byId('more').disabled = true;
    const generation = supportGeneration, requestedFact = fact;
    const abort = new AbortController(); supportAbort = abort;
    byId('support-status').textContent = reset ? 'Updating retained traces…' : 'Loading more traces…';
    byId('support-retry').hidden = true;
    try {
      const response = await fetch('/query/support', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({factKey: requestedFact, cursor: reset ? null : cursor}), signal: abort.signal
      });
      if (!response.ok) throw new Error('Could not load supporting traces. Retry the list.');
      const page = await response.json();
      if (generation !== supportGeneration || requestedFact !== fact) return;
      if (page.instance !== instance) { acceptInstance(page.instance); refreshSupport(); return; }
      if (reset) records = [];
      const known = new Set(records.map(keyOf));
      for (const record of page.traces) if (!known.has(keyOf(record))) {
        records.push(record); known.add(keyOf(record));
      }
      cursor = page.nextCursor;
      byId('fact-label').textContent = page.factLabel;
      byId('support-status').textContent = records.length
        ? `${records.length} retained trace${records.length === 1 ? '' : 's'} shown${cursor ? ' · more available' : ''}`
        : 'No supporting traces are currently retained.';
      renderRecords();
    } catch (error) {
      if (generation === supportGeneration && error.name !== 'AbortError') {
        byId('support-status').textContent = error.message;
        byId('support-retry').hidden = false;
      }
    } finally {
      if (generation === supportGeneration) {
        supportBusy = false; byId('more').disabled = false;
        if (supportPending) { supportPending = false; refreshSupport(); }
      }
    }
  }

  function selectFact(key, label) {
    fact = key; cursor = null; records = [];
    ++supportGeneration; supportAbort?.abort(); supportBusy = false; supportPending = false;
    byId('fact-label').textContent = label;
    renderRecords(); refreshSupport();
  }

  function highlight() {
    const keys = selection?.snapshot ? new Set(selection.snapshot.factKeys) : null;
    for (const element of graph.querySelectorAll('[data-fact]')) {
      element.classList.toggle('dimmed', Boolean(keys && !keys.has(element.dataset.fact)));
      element.classList.toggle('highlighted', Boolean(keys?.has(element.dataset.fact)));
    }
  }

  // Rebuild an inert subset rather than treating parsed SVG as trusted markup.
  function safeSvg(source) {
    const documentSvg = new DOMParser().parseFromString(source, 'image/svg+xml');
    if (documentSvg.querySelector('parsererror') || documentSvg.documentElement.localName !== 'svg')
      throw new Error('The service map could not be read. It will retry on the next update.');
    const tags = new Set(['svg', 'g', 'a', 'title', 'desc', 'path', 'polygon', 'polyline', 'ellipse', 'circle', 'rect', 'line', 'text', 'tspan']);
    const attributes = new Set(['id', 'class', 'transform', 'width', 'height', 'viewBox', 'fill', 'stroke',
      'stroke-width', 'stroke-dasharray', 'stroke-linecap', 'stroke-linejoin', 'points', 'd', 'x', 'y',
      'x1', 'x2', 'y1', 'y2', 'cx', 'cy', 'r', 'rx', 'ry', 'font-family', 'font-size', 'font-weight',
      'xlink:title', 'text-anchor', 'dominant-baseline', 'opacity', 'fill-opacity', 'stroke-opacity', 'xml:space']);
    const namespace = 'http://www.w3.org/2000/svg', ids = new Set();
    const root = document.createElementNS(namespace, 'svg');
    const pending = [[documentSvg.documentElement, root]];
    while (pending.length) {
      const [sourceElement, target] = pending.pop();
      for (const attribute of sourceElement.attributes) {
        if (!attributes.has(attribute.name) || /url\s*\(|javascript:/i.test(attribute.value)) continue;
        if (attribute.name === 'id') {
          if (!/^[A-Za-z_][\w:.-]*$/.test(attribute.value) || ids.has(attribute.value)) continue;
          ids.add(attribute.value);
        }
        target.setAttribute(attribute.name, attribute.value);
      }
      for (const child of sourceElement.childNodes) {
        if (child.nodeType === Node.TEXT_NODE) target.append(document.createTextNode(child.textContent));
        else if (child.nodeType === Node.ELEMENT_NODE && tags.has(child.localName) && child.namespaceURI === namespace) {
          const copy = document.createElementNS(namespace, child.localName);
          target.append(copy); pending.push([child, copy]);
        }
      }
    }
    for (const element of root.querySelectorAll('g[id^="fact_"]')) {
      if (!/^fact_[0-9a-f]+(?:_source|_destination)?$/.test(element.id)) continue;
      element.dataset.fact = element.id.replace(/_(source|destination)$/, '');
      if (element.dataset.fact !== element.id) continue;
      const label = element.querySelector('a')?.getAttribute('xlink:title')
        || [...element.querySelectorAll('text')].map(node => node.textContent).join(' ')
        || 'Confirmed dependency';
      element.setAttribute('role', 'button'); element.setAttribute('tabindex', '0');
      element.setAttribute('aria-label', `Supporting traces for ${label}`);
      element.addEventListener('click', () => selectFact(element.dataset.fact, label));
      element.addEventListener('keydown', event => {
        if (event.key === 'Enter' || event.key === ' ') {
          event.preventDefault(); selectFact(element.dataset.fact, label);
        }
      });
    }
    return root;
  }

  async function refreshGraph(update) {
    const version = BigInt(update.renderedVersion);
    if (renderedVersion !== null && version <= renderedVersion) return;
    const generation = ++graphGeneration;
    graphAbort?.abort(); graphAbort = new AbortController();
    if (!update.hasTelemetry) {
      renderedVersion = version; graph.replaceChildren(); byId('waiting').hidden = false; return;
    }
    try {
      const response = await fetch(`/graph.svg?v=${version}`, {signal: graphAbort.signal});
      if (!response.ok) throw new Error('The service map is unavailable. It will retry on the next update.');
      const svg = await response.text();
      const responseInstance = response.headers.get('X-Silhouette-Observation-Instance');
      const actualVersion = BigInt(response.headers.get('X-Silhouette-Rendered-Version'));
      if (generation !== graphGeneration || responseInstance !== instance
          || (renderedVersion !== null && actualVersion < renderedVersion)) return;
      const firstFrame = !graph.firstElementChild;
      graph.replaceChildren(safeSvg(svg)); renderedVersion = actualVersion;
      byId('waiting').hidden = true; byId('graph-error').hidden = true; highlight();
      if (firstFrame) revealTopology();
    } catch (error) {
      if (generation === graphGeneration && error.name !== 'AbortError') {
        byId('graph-error').textContent = error.message; byId('graph-error').hidden = false;
      }
    }
  }

  function flushTreeLoads(state) {
    if (state.flushing) return;
    state.flushing = true;
    for (let index = 0; index < state.pending.length; ++index) state.pending[index]();
    state.pending = []; state.flushing = false;
  }

  function appendTreeBatch(container, indices, snapshot, state, offset = 0) {
    const limit = offset === 0 ? Math.max(100, state.batches.get(container.dataset.listKey) ?? 100) : 100;
    const end = Math.min(offset + limit, indices.length);
    container.dataset.loadedCount = String(end);
    for (let position = offset; position < end; ++position) {
      const index = indices[position], span = snapshot.spans[index], key = state.keys[index];
      const item = document.createElement('li'), details = document.createElement('details');
      details.dataset.spanKey = key;
      const summary = document.createElement('summary');
      summary.append(text('span', span.service ?? 'Missing service identity', 'span-service'),
        text('span', ` · ${span.operation}`, 'operation'));
      const warnings = [];
      if (!['root', 'resolved'].includes(span.parentResolution)) warnings.push(`${span.parentResolution} parent`);
      if (span.duplicateSpanId) warnings.push('duplicate span ID');
      if (warnings.length) summary.append(text('span', warnings.join(' · '), 'diagnostic'));
      details.append(summary);
      const load = () => {
        if (!details.open || details.dataset.loaded) return;
        details.dataset.loaded = 'true';
        const metadata = text('div', `Span ${span.spanId}${span.parentSpanId ? ` · parent ${span.parentSpanId}` : ''}`, 'span-meta');
        const copy = text('button', 'Copy span ID'); copy.dataset.copySpan = 'true';
        copy.addEventListener('click', () => copyId(span.spanId));
        metadata.append(document.createElement('br'), copy); details.append(metadata);
        if (span.childIndices.length) {
          const children = document.createElement('ul'); children.className = 'tree-list';
          children.dataset.listKey = key;
          appendTreeBatch(children, span.childIndices, snapshot, state); details.append(children);
        }
        flushTreeLoads(state);
      };
      details.addEventListener('toggle', load);
      if (state.open.has(key)) { details.open = true; state.pending.push(load); }
      item.append(details); container.append(item);
    }
    if (end < indices.length) {
      const item = document.createElement('li'), more = text('button', `Show more (${indices.length - end} remaining)`);
      more.addEventListener('click', () => {
        const focused = document.activeElement === more;
        item.remove(); appendTreeBatch(container, indices, snapshot, state, end); flushTreeLoads(state);
        if (focused) container.children[end]?.querySelector('summary')?.focus();
      });
      item.append(more); container.append(item);
    }
  }

  function showDetail(snapshot, previous) {
    byId('trace-heading').textContent = 'Request trace';
    byId('trace-status').classList.remove('stale');
    byId('trace-status').textContent = `${snapshot.lifecycle} · last evidence ${age(snapshot.evidenceAgeMillis)}`;
    byId('trace-summary').textContent = `${snapshot.spanCount} retained ${snapshot.spanCount === 1 ? "span" : "spans"} · ${snapshot.serviceCount} observed ${snapshot.serviceCount === 1 ? "service" : "services"} · ${snapshot.traceId}`;
    byId('copy-trace').hidden = false;
    // Unrelated observations and lifecycle/age-only changes do not rebuild the
    // selected evidence or remove the control currently holding keyboard focus.
    if (previous?.publicationSequence === snapshot.publicationSequence) return;
    const oldTree = byId('trace-tree');
    const focused = document.activeElement?.closest('#trace-tree details');
    const focus = focused ? {key:focused.dataset.spanKey,
      copy:document.activeElement.hasAttribute('data-copy-span')} : null;
    const state = {open:new Set([...oldTree.querySelectorAll('details[open]')].map(node => node.dataset.spanKey)),
      batches:new Map([...oldTree.querySelectorAll('[data-list-key]')].map(node => [node.dataset.listKey, Number(node.dataset.loadedCount)])),
      keys:[], pending:[], flushing:false};
    // Browser-only identities use immutable span evidence plus an occurrence
    // ordinal for identical duplicates; reconstruction indices may move.
    const occurrences = new Map();
    state.keys = snapshot.spans.map(span => {
      const identity = JSON.stringify([span.spanId,span.parentSpanId,span.service,span.operation,
        span.startTimeUnixNano,span.endTimeUnixNano]);
      const ordinal = occurrences.get(identity) ?? 0;
      occurrences.set(identity, ordinal + 1); return identity + ':' + ordinal;
    });
    const positions = new Map(state.keys.map((key,index) => [key,index]));
    const restore = new Set(state.open); if (focus) restore.add(focus.key);
    const restored = new Set();
    for (const key of restore) {
      let index = positions.get(key);
      while (index !== undefined && index !== null && !restored.has(index)) {
        restored.add(index);
        const parent = snapshot.spans[index].parentIndex;
        const siblings = parent === null ? snapshot.topLevelIndices : snapshot.spans[parent].childIndices;
        const listKey = parent === null ? 'roots' : state.keys[parent];
        state.batches.set(listKey, Math.max(state.batches.get(listKey) ?? 100,
          Math.ceil((siblings.indexOf(index) + 1) / 100) * 100));
        if (parent !== null) state.open.add(state.keys[parent]);
        index = parent;
      }
    }
    const tree = document.createElement('ul'); tree.className = 'tree-list'; tree.dataset.listKey = 'roots';
    appendTreeBatch(tree, snapshot.topLevelIndices, snapshot, state); flushTreeLoads(state);
    oldTree.replaceChildren(tree);
    if (focus) {
      const node = [...tree.querySelectorAll('details')].find(value => value.dataset.spanKey === focus.key);
      (focus.copy ? node?.querySelector('[data-copy-span]') : node?.querySelector('summary'))?.focus();
    }
    highlight();
  }

  async function refreshDetail() {
    const target = selection;
    if (!target || target.stale) return;
    if (target.busy) { target.pending = true; return; }
    target.busy = true; target.abort = new AbortController();
    byId('trace-retry').hidden = true;
    try {
      const response = await fetch(`/query/trace/${target.traceId}/${target.incarnation}?instance=${encodeURIComponent(target.instance)}`,
        {signal: target.abort.signal});
      if (selection !== target || target.stale) return;
      if (response.status === 404) { freezeSelection('No longer retained. Showing the last snapshot.'); return; }
      if (!response.ok) throw new Error(target.snapshot
        ? 'Could not update this trace. The last snapshot is still shown.'
        : 'Could not load this trace. Retry the trace.');
      const snapshot = await response.json();
      if (selection !== target || target.stale || snapshot.instance !== target.instance
          || keyOf(snapshot) !== keyOf(target)) return;
      const previous = target.snapshot; target.snapshot = snapshot; showDetail(snapshot, previous);
    } catch (error) {
      if (selection === target && !target.stale && error.name !== 'AbortError') {
        byId('trace-status').textContent = error.message; byId('trace-retry').hidden = false;
      }
    } finally {
      target.busy = false;
      if (selection === target && target.pending && !target.stale) {
        target.pending = false; refreshDetail();
      }
    }
  }

  function openTrace(record) {
    selection?.abort?.abort();
    selection = {...record, instance, originatingFact:fact, stale: false, busy: false, pending: false, snapshot: null};
    byId('trace-status').classList.remove('stale'); byId('trace-status').textContent = 'Loading retained trace…';
    byId('trace-summary').textContent = ''; byId('trace-tree').replaceChildren();
    byId('copy-trace').hidden = true; byId('trace-retry').hidden = true; byId('copy-status').textContent = '';
    if (!dialog.open) dialog.showModal();
    renderRecords(); highlight(); refreshDetail();
  }

  async function copyId(value) {
    try { await navigator.clipboard.writeText(value); byId('copy-status').textContent = 'ID copied.'; }
    catch { byId('copy-status').textContent = 'Clipboard unavailable. Select the ID text to copy it.'; }
  }
  dialog.addEventListener('close', () => {
    const closed = selection;
    selection?.abort?.abort(); selection = null;
    byId('trace-tree').replaceChildren(); byId('trace-summary').textContent = '';
    highlight(); renderRecords();
    const row = closed?.instance === instance
      ? [...byId('traces').querySelectorAll('button')].find(value => value.dataset.recordKey === keyOf(closed)) : null;
    const origin = [...graph.querySelectorAll('[role=button]')].find(value => value.id === closed?.originatingFact);
    (row ?? origin ?? byId('support-heading')).focus();
  });
  byId('close-trace').addEventListener('click', () => dialog.close());
  byId('copy-trace').addEventListener('click', () => selection && copyId(selection.traceId));
  byId('trace-retry').addEventListener('click', refreshDetail);
  byId('support-retry').addEventListener('click', () => refreshSupport());
  byId('more').addEventListener('click', () => refreshSupport(false));

  const events = new EventSource('/events');
  events.onopen = () => { byId('status').textContent = 'LIVE'; };
  events.onerror = () => { byId('status').textContent = 'RECONNECTING'; };
  events.addEventListener('graph-updated', event => {
    const update = JSON.parse(event.data);
    acceptInstance(update.instance); refreshGraph(update);
  });
  events.addEventListener('observations-updated', event => {
    const update = JSON.parse(event.data);
    acceptInstance(update.instance);
    const version = BigInt(update.observationVersion);
    if (observationVersion !== null && version <= observationVersion) return;
    observationVersion = version; refreshSupport(); refreshDetail();
  });
  window.addEventListener('pagehide', () => {
    events.close(); supportAbort?.abort(); graphAbort?.abort(); selection?.abort?.abort(); selection = null;
  });
})();
