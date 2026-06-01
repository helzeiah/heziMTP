'use strict';

// ── Bridge ────────────────────────────────────────────────────────────────────
let _reqId = 0;
const _pending = {};
function call(action, params = {}) {
    return new Promise(resolve => {
        const id = ++_reqId;
        _pending[id] = resolve;
        try {
            window.webkit.messageHandlers.bridge.postMessage({id, action, ...params});
        } catch(e) {
            // Dev fallback
            setTimeout(() => { if (_pending[id]) { _pending[id](null); delete _pending[id]; } }, 100);
        }
    });
}
window.bridgeResponse = function(id, data) {
    if (_pending[id]) { _pending[id](data); delete _pending[id]; }
};

// ── State ─────────────────────────────────────────────────────────────────────
const S = {
    connected: false, deviceName: '',
    storages: [], activeStorageId: 0,
    remoteNavStack: [],
    localPath: '', localFiles: [],
    remoteFiles: [], remoteRefreshing: false,
    transfers: [],
    statusMessage: '', statusTimer: 0,
    selLocal:  new Set(),   // keys = file.path strings
    selRemote: new Set(),   // keys = String(file.handle)
    lastLocalIdx:  -1,      // for shift+click range
    lastRemoteIdx: -1,
};

// ── Smart-render fingerprints ──────────────────────────────────────────────────
// The file lists are only fully rebuilt when the files themselves change.
// Selection and toolbar updates run every poll without touching the table DOM,
// which eliminates the 400 ms hover-flash from innerHTML replacement.
let _localFingerprint  = '';
let _remoteFingerprint = '';

function fingerprintFiles(files) {
    // Fast hash: join paths/handles — changes on any navigation or refresh
    return files.map(f => f.path || f.handle || f.name).join('\0');
}

// ── Polling ───────────────────────────────────────────────────────────────────
async function poll() {
    try {
        const data = await call('get_state');
        if (data) {
            const sl = S.selLocal, sr = S.selRemote;
            const lli = S.lastLocalIdx, lri = S.lastRemoteIdx;
            Object.assign(S, data);
            S.selLocal = sl; S.selRemote = sr;
            S.lastLocalIdx = lli; S.lastRemoteIdx = lri;
            render();
        }
    } catch(e) {}
    setTimeout(poll, 400);
}

// ── Rendering ─────────────────────────────────────────────────────────────────
function render() {
    renderDeviceBar();
    renderLocalToolbar();
    renderLocalFiles();
    renderRemoteToolbar();
    renderRemoteFiles();
    renderTransfers();
    renderStatusBar();
}

function esc(s) {
    if (s == null) return '';
    const d = document.createElement('div');
    d.textContent = String(s);
    return d.innerHTML;
}

function fileColor(f) {
    if (f.isDir) return 'color-dir';
    const ext = (f.name.split('.').pop() || '').toLowerCase();
    if (['nsp','xci','nsz','xcz'].includes(ext)) return 'color-game';
    if (ext === 'nro') return 'color-hb';
    return 'color-file';
}

// ── File table builder ────────────────────────────────────────────────────────
const FILE_TABLE_HEADER = `<thead><tr>
    <th>Name</th><th class="col-size">Size</th><th class="col-date">Modified</th>
</tr></thead>`;

function buildFileRows(files, selSet, idKey) {
    return files.map((f, i) => {
        const key = String(f[idKey] != null ? f[idKey] : (f.path || f.name));
        const sel = selSet.has(key) ? ' selected' : '';
        const cc  = fileColor(f);
        return `<tr class="frow${sel}" data-idx="${i}" data-key="${esc(key)}" draggable="${!f.isDir}">
            <td class="col-name"><div class="fname-cell">
                <span class="dot ${cc}"></span>
                <span class="${cc}">${esc(f.name)}</span>
            </div></td>
            <td class="col-size">${f.isDir ? '' : esc(f.sizeStr)}</td>
            <td class="col-date">${esc(f.date)}</td>
        </tr>`;
    }).join('');
}

// ── Selection helpers ─────────────────────────────────────────────────────────
function applySelectionToDOM(container, selSet) {
    container.querySelectorAll('.frow').forEach(r => {
        r.classList.toggle('selected', selSet.has(r.dataset.key));
    });
}

function selectionKeys(files, idKey) {
    return files.map(f => String(f[idKey] != null ? f[idKey] : (f.path || f.name)));
}

// ── Local panel ───────────────────────────────────────────────────────────────
function renderLocalFiles() {
    const el = document.getElementById('local-files');
    const fp = fingerprintFiles(S.localFiles);

    if (fp !== _localFingerprint) {
        _localFingerprint = fp;
        // Full rebuild only when file list actually changed
        if (!S.localFiles.length) {
            el.innerHTML = `<div class="empty-state">
                <svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="1.5" class="empty-icon">
                    <path d="M6 38V14a2 2 0 012-2h12l4 4h16a2 2 0 012 2v20a2 2 0 01-2 2H8a2 2 0 01-2-2z"/>
                </svg>
                <div class="empty-title">No files in this folder</div></div>`;
            return;
        }
        el.innerHTML = `<table class="ftable">${FILE_TABLE_HEADER}
            <tbody>${buildFileRows(S.localFiles, S.selLocal, 'path')}</tbody></table>`;
        attachLocalEvents(el);
    } else {
        // Cheap path: just sync selection highlight
        applySelectionToDOM(el, S.selLocal);
    }
}

function attachLocalEvents(el) {
    const files = S.localFiles;
    const keys  = selectionKeys(files, 'path');

    el.querySelectorAll('.frow').forEach((row, i) => {
        const f   = files[i];
        const key = keys[i];

        // Click → select
        row.addEventListener('click', e => {
            handleRowClick(e, i, key, S.selLocal, keys, 'lastLocalIdx');
            applySelectionToDOM(el, S.selLocal);
        });

        // Double-click → navigate dir OR upload file to Switch
        row.addEventListener('dblclick', () => {
            if (f.isDir) { call('navigate_local', {path: f.path}); }
            else if (S.connected) { call('start_upload', {srcPath: f.path}); }
        });

        // Right-click → local context menu
        row.addEventListener('contextmenu', e => {
            e.preventDefault();
            showLocalCtxMenu(f, e);
        });

        // Drag source: drag selected local file(s) to the remote panel
        if (!f.isDir) {
            row.addEventListener('dragstart', e => {
                // Drag all selected files if this one is in the selection, else just this
                const dragFiles = S.selLocal.has(key)
                    ? files.filter((_, j) => S.selLocal.has(keys[j]))
                    : [f];
                e.dataTransfer.setData('application/x-hezi-local',
                    JSON.stringify(dragFiles.map(df => ({path: df.path, name: df.name}))));
                e.dataTransfer.effectAllowed = 'copy';
                e.dataTransfer.setDragImage(dragBadge(dragFiles.length, dragFiles[0].name), 0, 0);
            });
        }
    });

    // Drop target: receive remote files dropped here → download
    el.addEventListener('dragover', e => { e.preventDefault(); el.classList.add('drag-over'); });
    el.addEventListener('dragleave', e => { if (!el.contains(e.relatedTarget)) el.classList.remove('drag-over'); });
    el.addEventListener('drop', e => {
        e.preventDefault();
        el.classList.remove('drag-over');
        const d = e.dataTransfer.getData('application/x-hezi-remote');
        if (d) {
            const items = JSON.parse(d);
            (Array.isArray(items) ? items : [items]).forEach(rf => {
                call('start_download', {handle: rf.handle, storageId: rf.storageId, filename: rf.name, size: rf.size});
            });
        }
    });
}

// ── Remote panel ──────────────────────────────────────────────────────────────
function renderRemoteFiles() {
    const el    = document.getElementById('remote-files');
    const empty = document.getElementById('remote-empty-state');

    if (!S.connected) {
        if (empty) empty.style.display = '';
        el.querySelectorAll('table,.loading,.empty-state:not(#remote-empty-state)')
            .forEach(n => n.remove());
        _remoteFingerprint = '';
        return;
    }
    if (empty) empty.style.display = 'none';

    if (S.remoteRefreshing) {
        el.innerHTML = '<div class="loading">Loading…</div>';
        _remoteFingerprint = '__loading__';
        return;
    }

    const fp = fingerprintFiles(S.remoteFiles);
    if (fp !== _remoteFingerprint) {
        _remoteFingerprint = fp;
        if (!S.remoteFiles.length) {
            el.innerHTML = `<div class="empty-state">
                <svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="1.5" class="empty-icon">
                    <path d="M6 38V14a2 2 0 012-2h12l4 4h16a2 2 0 012 2v20a2 2 0 01-2 2H8a2 2 0 01-2-2z"/>
                </svg>
                <div class="empty-title">Empty folder</div></div>`;
            return;
        }
        el.innerHTML = `<table class="ftable">${FILE_TABLE_HEADER}
            <tbody>${buildFileRows(S.remoteFiles, S.selRemote, 'handle')}</tbody></table>`;
        attachRemoteEvents(el);
    } else {
        applySelectionToDOM(el, S.selRemote);
    }
}

function attachRemoteEvents(el) {
    const files = S.remoteFiles;
    const keys  = selectionKeys(files, 'handle');

    el.querySelectorAll('.frow').forEach((row, i) => {
        const f   = files[i];
        const key = keys[i];

        row.addEventListener('click', e => {
            handleRowClick(e, i, key, S.selRemote, keys, 'lastRemoteIdx');
            applySelectionToDOM(el, S.selRemote);
        });

        row.addEventListener('dblclick', () => {
            if (f.isDir) call('navigate_remote', {handle: f.handle, storageId: f.storageId, name: f.name});
        });

        row.addEventListener('contextmenu', e => { e.preventDefault(); showRemoteCtxMenu(f, e); });

        // Drag source: drag selected remote file(s) to the local panel
        if (!f.isDir) {
            row.addEventListener('dragstart', e => {
                const dragFiles = S.selRemote.has(key)
                    ? files.filter((_, j) => S.selRemote.has(keys[j]))
                    : [f];
                e.dataTransfer.setData('application/x-hezi-remote',
                    JSON.stringify(dragFiles.map(df => ({handle: df.handle, storageId: df.storageId, name: df.name, size: df.size}))));
                e.dataTransfer.effectAllowed = 'copy';
                e.dataTransfer.setDragImage(dragBadge(dragFiles.length, dragFiles[0].name), 0, 0);
            });
        }
    });

    // Drop target: receive local files → upload
    el.addEventListener('dragover', e => { e.preventDefault(); el.classList.add('drag-over'); });
    el.addEventListener('dragleave', e => { if (!el.contains(e.relatedTarget)) el.classList.remove('drag-over'); });
    el.addEventListener('drop', e => {
        e.preventDefault();
        el.classList.remove('drag-over');
        // Internal drag (local app files)
        const d = e.dataTransfer.getData('application/x-hezi-local');
        if (d) {
            const items = JSON.parse(d);
            (Array.isArray(items) ? items : [items]).forEach(lf => {
                call('start_upload', {srcPath: lf.path});
            });
            return;
        }
        // Finder drop
        for (const file of (e.dataTransfer.files || [])) {
            const p = file.path || '';
            if (p) call('start_upload', {srcPath: p});
        }
    });
}

// ── Shared row click with shift-range support ─────────────────────────────────
function handleRowClick(e, idx, key, selSet, allKeys, lastIdxProp) {
    if (e.shiftKey && S[lastIdxProp] >= 0) {
        const lo = Math.min(idx, S[lastIdxProp]);
        const hi = Math.max(idx, S[lastIdxProp]);
        if (!e.metaKey) selSet.clear();
        allKeys.slice(lo, hi + 1).forEach(k => selSet.add(k));
    } else if (e.metaKey) {
        if (selSet.has(key)) selSet.delete(key); else selSet.add(key);
        S[lastIdxProp] = idx;
    } else {
        selSet.clear();
        selSet.add(key);
        S[lastIdxProp] = idx;
    }
}

// ── Drag badge ghost image ─────────────────────────────────────────────────────
function dragBadge(count, firstName) {
    const el = document.createElement('div');
    el.style.cssText = [
        'position:fixed', 'top:-200px', 'left:-200px',
        'background:rgba(10,132,255,0.85)', 'color:#fff',
        'font:600 12px -apple-system', 'padding:4px 10px',
        'border-radius:10px', 'white-space:nowrap',
    ].join(';');
    el.textContent = count > 1 ? `${count} files` : firstName;
    document.body.appendChild(el);
    setTimeout(() => el.remove(), 0);
    return el;
}

// ── Toolbars ──────────────────────────────────────────────────────────────────
function renderLocalToolbar() {
    const crumb = document.getElementById('local-breadcrumb');
    const parts = (S.localPath || '').split('/').filter(Boolean);
    const show  = parts.slice(-4);
    let html    = show.length < parts.length ? '<span class="crumb-ellipsis">…</span><span class="crumb-sep">›</span>' : '';
    show.forEach((p, i) => {
        const fullIdx = parts.length - show.length + i;
        const path    = '/' + parts.slice(0, fullIdx + 1).join('/');
        html += i === show.length - 1
            ? `<span class="crumb-cur">${esc(p)}</span>`
            : `<span class="crumb-link" data-path="${esc(path)}">${esc(p)}</span><span class="crumb-sep">›</span>`;
    });
    crumb.innerHTML = html;
    crumb.querySelectorAll('.crumb-link').forEach(el =>
        el.addEventListener('click', () => call('navigate_local', {path: el.dataset.path})));
    document.getElementById('btn-local-up').disabled = parts.length === 0;
}

function renderRemoteToolbar() {
    const sel = document.getElementById('storage-sel');
    if (S.storages && S.storages.length) {
        const prev = sel.value;
        sel.innerHTML = S.storages.map(s =>
            `<option value="${s.id}">${esc(s.name)}${s.freeStr ? ' (' + s.freeStr + ')' : ''}</option>`
        ).join('');
        sel.value = String(S.activeStorageId || prev);
        sel.style.display = '';
    } else {
        sel.style.display = 'none';
    }
    const stack = S.remoteNavStack || [];
    const crumb = document.getElementById('remote-breadcrumb');
    crumb.innerHTML = stack.slice(1).map((e, i) => {
        const absIdx = i + 1;
        return absIdx === stack.length - 1
            ? `<span class="crumb-cur">${esc(e.label)}</span>`
            : `<span class="crumb-link" data-idx="${absIdx}">${esc(e.label)}</span><span class="crumb-sep">›</span>`;
    }).join('');
    crumb.querySelectorAll('.crumb-link').forEach(el =>
        el.addEventListener('click', () => call('navigate_remote_to', {index: +el.dataset.idx})));
    document.getElementById('btn-remote-up').disabled = stack.length <= 1;
}

// ── Device bar ────────────────────────────────────────────────────────────────
function renderDeviceBar() {
    const badge  = document.getElementById('conn-badge');
    const status = document.getElementById('device-status-text');
    if (S.connected) {
        badge.className  = 'badge connected';
        badge.textContent = S.deviceName || 'Nintendo Switch';
        status.textContent = S.deviceName || 'Nintendo Switch connected';
    } else {
        badge.className  = 'badge disconnected';
        badge.textContent = 'No Device';
        status.textContent = 'Waiting for Nintendo Switch…';
    }
}

// ── Transfers panel ───────────────────────────────────────────────────────────
function renderTransfers() {
    const list     = document.getElementById('transfers-list');
    const label    = document.getElementById('transfers-label');
    const cancelAll= document.getElementById('btn-cancel-all');
    const panel    = document.getElementById('transfers-panel');

    const active = (S.transfers || []).filter(t => t.state <= 1);
    label.textContent = active.length ? `${active.length} Transfer${active.length > 1 ? 's' : ''}` : 'Transfers';
    cancelAll.style.display = active.length ? '' : 'none';

    if (!S.transfers || !S.transfers.length) { list.innerHTML = ''; panel.style.display = 'none'; return; }
    panel.style.display = '';

    const stateNames = ['Queued','','Done','Failed','Cancelled'];
    list.innerHTML = S.transfers.map(t => {
        const dir      = t.direction === 0 ? 'DL' : 'UP';
        const dc       = t.direction === 0 ? 'dir-dl' : 'dir-ul';
        const pct      = ((t.progress || 0) * 100).toFixed(0);
        const showBar  = t.state <= 1;
        const statText = t.state === 1 ? esc(t.speedStr)
                        : t.state === 3 ? '⚠ ' + esc(t.error || 'Failed')
                        : stateNames[t.state] || '';
        return `<div class="txfer-item">
            <div class="txfer-row">
                <span class="txfer-dir ${dc}">${dir}</span>
                <span class="txfer-name" title="${esc(t.filename)}">${esc(t.filename)}</span>
                <span class="txfer-stat">${statText}</span>
                ${showBar ? `<button class="txfer-cancel" data-id="${esc(t.id)}">×</button>` : ''}
            </div>
            ${showBar ? `<div class="pbar"><div class="pbar-fill" style="width:${pct}%"></div></div>
            <div class="txfer-meta">${pct}%${t.etaStr ? '  ·  ' + t.etaStr : ''}</div>` : ''}
        </div>`;
    }).join('');
    list.querySelectorAll('.txfer-cancel').forEach(btn =>
        btn.addEventListener('click', () => call('cancel_transfer', {id: btn.dataset.id})));
}

// ── Status bar ────────────────────────────────────────────────────────────────
function renderStatusBar() {
    const lf = (S.localFiles  || []).length;
    const rf = (S.remoteFiles || []).length;
    document.getElementById('sb-left').textContent  = `${lf} item${lf !== 1 ? 's' : ''}`;
    document.getElementById('sb-right').textContent = S.connected ? `${rf} item${rf !== 1 ? 's' : ''}` : '';
    let center = '';
    if (S.statusTimer > 0 && S.statusMessage) {
        center = S.statusMessage;
    } else if (S.connected) {
        const act = (S.transfers || []).filter(t => t.state === 1);
        if (act.length) {
            const spd = act.reduce((s, t) => s + (t.speedBps || 0), 0);
            center = `${act.length} active · ` + (spd > 1048576
                ? (spd/1048576).toFixed(0) + ' MB/s'
                : (spd/1024).toFixed(0) + ' KB/s');
        }
    }
    document.getElementById('sb-center').textContent = center;
}

// ── Context menus ─────────────────────────────────────────────────────────────
let _ctxFile = null, _ctxIsLocal = false;

function showLocalCtxMenu(file, e) {
    _ctxFile    = file;
    _ctxIsLocal = true;
    const m = document.getElementById('ctx-menu');
    document.getElementById('ctx-upload').style.display   = (!file.isDir && S.connected) ? '' : 'none';
    document.getElementById('ctx-download').style.display = 'none';
    document.getElementById('ctx-delete').style.display   = 'none';
    document.getElementById('ctx-sep-del').style.display  = 'none';
    m.style.left = e.clientX + 'px';
    m.style.top  = e.clientY + 'px';
    m.classList.remove('hidden');
}

function showRemoteCtxMenu(file, e) {
    _ctxFile    = file;
    _ctxIsLocal = false;
    const m = document.getElementById('ctx-menu');
    document.getElementById('ctx-upload').style.display   = 'none';
    document.getElementById('ctx-download').style.display = (!file.isDir) ? '' : 'none';
    document.getElementById('ctx-delete').style.display   = '';
    document.getElementById('ctx-sep-del').style.display  = '';
    m.style.left = e.clientX + 'px';
    m.style.top  = e.clientY + 'px';
    m.classList.remove('hidden');
}

document.addEventListener('click',       () => document.getElementById('ctx-menu').classList.add('hidden'));
document.addEventListener('contextmenu', e  => { if (!e.target.closest('.frow')) { e.preventDefault(); document.getElementById('ctx-menu').classList.add('hidden'); } });

// ── Init ──────────────────────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', () => {
    document.getElementById('btn-local-up')     .addEventListener('click', () => call('navigate_local_up'));
    document.getElementById('btn-local-home')   .addEventListener('click', () => call('navigate_local', {path: '~'}));
    document.getElementById('btn-local-refresh').addEventListener('click', () => call('refresh_local'));
    document.getElementById('btn-remote-up')    .addEventListener('click', () => call('navigate_remote_up'));
    document.getElementById('btn-remote-refresh').addEventListener('click', () => call('refresh_remote'));
    document.getElementById('btn-scan')         .addEventListener('click', () => call('reconnect'));
    document.getElementById('btn-open-picker')  .addEventListener('click', () => call('open_file_picker'));
    document.getElementById('btn-cancel-all')   .addEventListener('click', () => call('cancel_all'));
    document.getElementById('storage-sel')      .addEventListener('change', e => call('set_storage', {storageId: +e.target.value}));

    // Context menu actions
    document.getElementById('ctx-upload').addEventListener('click', () => {
        if (!_ctxFile) return;
        const uploadFiles = S.selLocal.size > 1 && S.selLocal.has(_ctxFile.path)
            ? S.localFiles.filter(f => S.selLocal.has(f.path))
            : [_ctxFile];
        uploadFiles.forEach(f => call('start_upload', {srcPath: f.path}));
    });
    document.getElementById('ctx-download').addEventListener('click', () => {
        if (!_ctxFile) return;
        const downloadFiles = S.selRemote.size > 1 && S.selRemote.has(String(_ctxFile.handle))
            ? S.remoteFiles.filter(f => S.selRemote.has(String(f.handle)))
            : [_ctxFile];
        downloadFiles.forEach(f => call('start_download', {handle: f.handle, storageId: f.storageId, filename: f.name, size: f.size}));
    });
    document.getElementById('ctx-delete').addEventListener('click', () => {
        if (!_ctxFile) return;
        const label = S.selRemote.size > 1 && S.selRemote.has(String(_ctxFile.handle))
            ? `${S.selRemote.size} items`
            : `"${_ctxFile.name}"`;
        if (window.confirm(`Delete ${label}?`)) {
            const handles = S.selRemote.size > 1 && S.selRemote.has(String(_ctxFile.handle))
                ? S.remoteFiles.filter(f => S.selRemote.has(String(f.handle))).map(f => f.handle)
                : [_ctxFile.handle];
            handles.forEach(h => call('delete_remote', {handle: h}));
        }
    });

    // Keyboard
    document.addEventListener('keydown', e => {
        if (e.target.matches('input,textarea,select')) return;
        if (e.key === 'Backspace' || e.key === 'Delete') {
            S.selRemote.forEach(h => call('delete_remote', {handle: +h}));
        }
        if (e.metaKey && e.key === 'r') {
            e.preventDefault();
            call('refresh_remote');
            call('refresh_local');
        }
        // Cmd+A = select all
        if (e.metaKey && e.key === 'a') {
            e.preventDefault();
            const active = document.activeElement;
            if (active && active.closest('#local-files')) {
                S.localFiles.forEach(f => S.selLocal.add(f.path));
                applySelectionToDOM(document.getElementById('local-files'), S.selLocal);
            } else {
                S.remoteFiles.forEach(f => S.selRemote.add(String(f.handle)));
                applySelectionToDOM(document.getElementById('remote-files'), S.selRemote);
            }
        }
        // Escape = deselect
        if (e.key === 'Escape') {
            S.selLocal.clear(); S.selRemote.clear();
            applySelectionToDOM(document.getElementById('local-files'),  S.selLocal);
            applySelectionToDOM(document.getElementById('remote-files'), S.selRemote);
        }
    });

    poll();
});
