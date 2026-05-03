// Dev shim: when opened locally (file://, localhost), mocks /list and
// /annotations so the gallery renders without the device.
(() => {
  const isDev = location.protocol === 'file:'
              || location.hostname === 'localhost'
              || location.hostname === '127.0.0.1';
  if (!isDev) return;
  document.body.classList.add('dev');
  document.getElementById('devbar').hidden = false;

  const MOCK_LIST = {
    photos: ['img_000001.jpg', 'img_000002.jpg', 'img_000003.jpg',
             'img_000004.jpg', 'img_000005.jpg', 'img_000006.jpg'],
    audio:  ['rec_000001.wav', 'rec_000002.wav'],
  };
  const MOCK_ANNOT = {
    'img_000002.jpg': {name: 'Robert Downey Jr', dist: 0.412},
    'img_000004.jpg': {name: 'Chris Evans',      dist: 0.566},
    'img_000005.jpg': {name: 'Mark Ruffalo',     dist: 0.310},
  };

  const origFetch = window.fetch;
  window.fetch = (input, init) => {
    const url = typeof input === 'string' ? input : input.url;
    const json = (data) => new Response(JSON.stringify(data),
        {status: 200, headers: {'Content-Type': 'application/json'}});
    if (url.endsWith('/list'))        return Promise.resolve(json(MOCK_LIST));
    if (url.endsWith('/annotations')) return Promise.resolve(json(MOCK_ANNOT));
    if ((init?.method === 'DELETE') &&
        (url.includes('/photo/') || url.includes('/audio/'))) {
      console.log('[dev] mock DELETE', url);
      return Promise.resolve(new Response('', {status: 204}));
    }
    return origFetch(input, init);
  };
})();

function makeToolbar(card, url, name, recountFn) {
  const t = document.createElement('div'); t.className = 'toolbar';
  const dl = document.createElement('a');
  dl.className = 'dl'; dl.href = url; dl.download = name;
  dl.title = 'download'; dl.textContent = '↓';
  dl.addEventListener('click', e => e.stopPropagation());
  const del = document.createElement('button');
  del.className = 'del'; del.type = 'button';
  del.title = 'delete'; del.textContent = '✕';
  del.addEventListener('click', async (e) => {
    e.preventDefault(); e.stopPropagation();
    if (!confirm('Delete ' + name + '?')) return;
    del.disabled = true;
    try {
      const r = await fetch(url, {method: 'DELETE'});
      if (r.ok) { card.remove(); recountFn(); }
      else { alert('delete failed: HTTP ' + r.status); del.disabled = false; }
    } catch (err) { alert('delete failed: ' + err); del.disabled = false; }
  });
  t.appendChild(dl); t.appendChild(del);
  return t;
}

function makeMeta(card, url, name, recountFn) {
  const m = document.createElement('div'); m.className = 'meta';
  const cap = document.createElement('span'); cap.className = 'name';
  cap.textContent = name;
  m.appendChild(cap);
  m.appendChild(makeToolbar(card, url, name, recountFn));
  return m;
}

function makePhotoCard(n, recountFn, annot) {
  const url = '/photo/' + encodeURIComponent(n);
  const card = document.createElement('div'); card.className = 'card';
  const a = document.createElement('a'); a.className = 'view';
  a.href = url; a.target = '_blank';
  const img = document.createElement('img');
  img.src = url; img.loading = 'lazy'; img.alt = n;
  a.appendChild(img);
  card.appendChild(a);
  card.appendChild(makeMeta(card, url, n, recountFn));
  if (annot && annot.name) {
    const conf = Math.max(0, Math.min(100, Math.round((1 - annot.dist) * 100)));
    const tag = document.createElement('div');
    tag.className = 'annot';
    tag.textContent = annot.name + ' — ' + conf + '% match';
    card.appendChild(tag);
  }
  return card;
}

function makeAudioCard(n, recountFn) {
  const url = '/audio/' + encodeURIComponent(n);
  const card = document.createElement('div'); card.className = 'card';
  const player = document.createElement('audio');
  player.controls = true; player.preload = 'metadata'; player.src = url;
  card.appendChild(player);
  card.appendChild(makeMeta(card, url, n, recountFn));
  return card;
}

async function refresh() {
  try {
    const [listR, annotR] = await Promise.all([
      fetch('/list', {cache: 'no-store'}),
      fetch('/annotations', {cache: 'no-store'}),
    ]);
    const data = await listR.json();
    const annot = await annotR.json();
    const gp = document.getElementById('gp');
    const ga = document.getElementById('ga');
    const pc = document.getElementById('pc');
    const ac = document.getElementById('ac');
    const recount = () => {
      pc.textContent = '(' + Array.from(gp.children).filter(c => !c.classList.contains('empty')).length + ')';
      ac.textContent = '(' + Array.from(ga.children).filter(c => !c.classList.contains('empty')).length + ')';
    };
    gp.replaceChildren();
    ga.replaceChildren();
    if (!data.photos.length) {
      const d = document.createElement('div'); d.className = 'empty';
      d.textContent = 'no photos yet — short-press BOOT to capture';
      gp.appendChild(d);
    } else {
      for (const n of data.photos) gp.appendChild(makePhotoCard(n, recount, annot[n]));
    }
    if (!data.audio.length) {
      const d = document.createElement('div'); d.className = 'empty';
      d.textContent = 'no audio yet — long-press BOOT (≥2.5s) to start, again to stop';
      ga.appendChild(d);
    } else {
      for (const n of data.audio) ga.appendChild(makeAudioCard(n, recount));
    }
    pc.textContent = '(' + data.photos.length + ')';
    ac.textContent = '(' + data.audio.length + ')';
  } catch (e) { console.error(e); }
}

refresh();
setInterval(refresh, 5000);
