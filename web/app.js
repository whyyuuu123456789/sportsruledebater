"use strict";

// The Sports Rules Council's page. Everything the AI writes is inserted as text, never as HTML.

const $ = (id) => document.getElementById(id);
const HEADERS = { "X-Requested-With": "council" };
const YOUTUBE = /(?:youtube\.com\/(?:watch\?(?:.*&)?v=|shorts\/|embed\/|live\/)|youtu\.be\/)([\w-]{11})/;
const VIDEO_FILE = /\.(mp4|webm|mov|mkv|m4v|avi|ogv|ogg)(?:[?#]|$)/i;
const ORDER = ["queued", "downloading", "watching", "building", "checking", "discussing", "done"];
const STEPS = [
  ["downloading", "Get the clip", (v) => v.has_clip],
  ["watching", "Read the frames", (v) => v.has_clip],
  ["building", "Build the case", () => true],
  ["checking", "Your check", (v) => v.check],
  ["discussing", "Discussion and panel vote", () => true],
];

let config = { yt_dlp: false, rounds: 4 };
let file = null;    // a clip picked for upload
let player = null;  // { time() } for whatever is in the preview
let review = null;  // { id, next, lastStatus, checkShown }
let pollTimer = null;

// ---- small helpers ----
function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (key === "class") node.className = value;
    else if (key === "text") node.textContent = value;
    else if (key.startsWith("on")) node.addEventListener(key.slice(2), value);
    else if (value === true) node.setAttribute(key, "");
    else if (value !== false && value !== null && value !== undefined) node.setAttribute(key, value);
  }
  for (const child of children.flat(Infinity)) {
    if (child === null || child === undefined || child === false) continue;
    node.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  return node;
}

function clock(seconds) {
  const m = Math.floor(seconds / 60);
  return `${m}:${(seconds - m * 60).toFixed(1).padStart(4, "0")}`;
}

function debounce(fn, ms) {
  let timer;
  return (...args) => { clearTimeout(timer); timer = setTimeout(() => fn(...args), ms); };
}

async function api(path, options = {}) {
  const resp = await fetch(path, options);
  const data = await resp.json().catch(() => ({}));
  if (!resp.ok) throw new Error(data.error || `The server answered ${resp.status}.`);
  return data;
}

function postJson(path, body) {
  return api(path, { method: "POST", headers: { ...HEADERS, "Content-Type": "application/json" },
                     body: JSON.stringify(body) });
}

// ---- the preview player and timestamps ----
function setTimeButtons(on) {
  $("set-start").disabled = !on;
  $("set-end").disabled = !on;
}

function clearPlayer() {
  $("player").replaceChildren();
  $("player").hidden = true;
  player = null;
  setTimeButtons(false);
}

function showVideo(src) {
  const video = el("video", { src, controls: true, preload: "metadata", playsinline: true });
  $("player").replaceChildren(video);
  $("player").hidden = false;
  player = { time: () => video.currentTime };
  setTimeButtons(true);
}

let youTubeApi = null;
function loadYouTubeApi() {
  youTubeApi ??= new Promise((resolve) => {
    window.onYouTubeIframeAPIReady = resolve;
    document.head.append(el("script", { src: "https://www.youtube.com/iframe_api" }));
  });
  return youTubeApi;
}

async function showYouTube(id) {
  const holder = el("div");
  $("player").replaceChildren(holder);
  $("player").hidden = false;
  player = null;
  setTimeButtons(false);
  await loadYouTubeApi();
  const yt = new YT.Player(holder, {
    videoId: id,
    playerVars: { rel: 0 },
    events: { onReady: () => { player = { time: () => yt.getCurrentTime() }; setTimeButtons(true); } },
  });
}

function note(text, warn = false) {
  const box = $("link-note");
  box.textContent = text;
  box.className = warn ? "note warn" : "note";
  box.hidden = !text;
}

function showLink() {
  const url = $("url").value.trim();
  note("");
  if (!url) {
    if (!file) clearPlayer();
    return;
  }
  file = null;
  $("file").value = "";
  $("file-name").textContent = "";
  const yt = url.match(YOUTUBE);
  if (yt) {
    showYouTube(yt[1]);
    if (!config.yt_dlp) {
      note("You can watch YouTube links here to find the timestamps, but downloading them needs yt-dlp on this " +
           "computer (brew install yt-dlp). Check YouTube's terms and the footage's copyright first, or upload the " +
           "clip instead.", true);
    }
  } else if (VIDEO_FILE.test(url)) {
    showVideo(url);
  } else if (/^https?:\/\//i.test(url)) {
    clearPlayer();
    note(config.yt_dlp
      ? "If this links straight to a video file it's downloaded as it is; if it's a page with a video on it, yt-dlp fetches it."
      : "If this links straight to a video file, it'll work. If it's a web page with a video on it, the server needs yt-dlp (brew install yt-dlp).");
  } else {
    clearPlayer();
  }
}

// ---- asking for a ruling ----
async function submitAsk(event) {
  event.preventDefault();
  $("form-error").textContent = "";
  const go = $("go");
  go.disabled = true;
  try {
    let source = $("url").value.trim();
    if (file) {
      go.textContent = "Uploading…";
      source = (await api(`/api/upload?name=${encodeURIComponent(file.name)}`,
                          { method: "POST", headers: HEADERS, body: file })).source;
    }
    const { id } = await postJson("/api/reviews", {
      source, context: $("context").value, start: $("start").value, end: $("end").value,
      rounds: $("rounds").value, panel: $("panel").value, frames: $("frames").value, check: $("check").checked,
    });
    startProgress(id);
  } catch (err) {
    $("form-error").textContent = err.message;
  } finally {
    go.disabled = false;
    go.textContent = "Get a ruling";
  }
}

function startProgress(id) {
  review = { id, next: 0, lastStatus: "queued", checkShown: false };
  $("ask").hidden = true;
  $("check-case").hidden = true;
  $("ruling").hidden = true;
  $("progress").hidden = false;
  $("log").textContent = "";
  $("progress-error").textContent = "";
  $("steps").replaceChildren();
  poll();
}

async function poll() {
  clearTimeout(pollTimer);
  try {
    const view = await api(`/api/reviews/${review.id}?since=${review.next}`);
    review.next = view.next;
    if (view.lines.length) {
      const log = $("log");
      const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 40;
      log.textContent += view.lines.join("\n") + "\n";
      if (atBottom) log.scrollTop = log.scrollHeight;
    }
    $("progress-error").textContent = "";
    if (view.status !== "failed") review.lastStatus = view.status;
    drawSteps(view);
    if (view.status === "checking" && !review.checkShown) {
      review.checkShown = true;
      drawCheck(view);
    }
    if (view.status === "done") return drawRuling(view);
    if (view.status === "failed") {
      $("check-case").hidden = true;
      $("progress-error").textContent = view.error;
      $("progress").append(againButton());
      return;
    }
  } catch (err) {
    $("progress-error").textContent = `Lost touch with the server (${err.message}). Trying again…`;
  }
  pollTimer = setTimeout(poll, 1500);
}

function drawSteps(view) {
  const at = ORDER.indexOf(review.lastStatus);
  const items = STEPS.filter(([, , applies]) => applies(view)).map(([status, label]) => {
    const i = ORDER.indexOf(status);
    const state = view.status === "done" || i < at ? "done" : i === at && view.status !== "failed" ? "now" : "";
    return el("li", { class: state, text: label });
  });
  if (view.status === "queued") items.unshift(el("li", { class: "now", text: "Waiting for the review ahead of this one" }));
  $("steps").replaceChildren(...items);
}

// ---- checking the case ----
function editableList(prefix, values) {
  const list = el("div", { class: "list" });
  const renumber = () => [...list.children].forEach((row, i) => { row.firstChild.textContent = `${prefix}${i + 1}`; });
  const add = (text = "") => {
    const row = el("div", { class: "item" },
      el("span", { class: "tag" }),
      el("textarea", { rows: 2 }, text),
      el("button", { type: "button", class: "button secondary small", text: "Remove",
                     onclick: () => { row.remove(); renumber(); } }));
    list.append(row);
    renumber();
  };
  values.forEach((value) => add(value));
  return {
    node: el("div", {}, list, el("button", { type: "button", class: "button secondary small", text: "Add one",
                                             onclick: () => add() })),
    values: () => [...list.querySelectorAll("textarea")].map((t) => t.value.trim()).filter(Boolean),
  };
}

function numbered(prefix, values) {
  return Object.fromEntries(values.map((value, i) => [`${prefix}${i + 1}`, value]));
}

function likesWithFirst(order, first) {
  const ranked = [first, ...order.filter((r) => r !== first)];
  return Object.fromEntries(ranked.map((r, i) => [r, ranked.length > 1 ? +(1 - i / (ranked.length - 1)).toFixed(2) : 1]));
}

function framesStrip(urls, seen) {
  return el("div", { class: "frames" }, urls.map((url) => {
    const t = Number((url.match(/frame_([\d.]+)\.jpg$/) || [])[1] || 0);
    return el("figure", { class: seen.includes(url) ? "seen" : "" },
      el("img", { src: url, alt: `Frame at ${clock(t)}`, loading: "lazy" }),
      el("figcaption", { text: clock(t) }));
  }));
}

function drawCheck(view) {
  const c = view.case;
  const form = $("check-case");
  const incident = el("textarea", { rows: 3 }, c.incident);
  const rulingInputs = Object.entries(c.rulings).map(([id, text]) => [id, el("input", { value: text })]);
  const call = el("select", {}, Object.keys(c.rulings).map((id) => el("option", { value: id, selected: id === c.call }, id)));
  const facts = editableList("F", Object.values(c.facts));
  const disputed = editableList("D", Object.values(c.disputed));
  const parties = c.parties.map((p) => {
    const order = Object.keys(p.likes).sort((a, b) => p.likes[b] - p.likes[a]);
    const select = el("select", {}, Object.keys(c.rulings).map((id) => el("option", { value: id, selected: id === order[0] }, id)));
    return { p, order, select };
  });
  const { rule_titles: titles, frame_urls: frames, ...plain } = c;
  const json = el("textarea", { rows: 14, class: "log" }, JSON.stringify(plain, null, 2));
  const original = json.value;
  const error = el("p", { class: "error", role: "alert" });

  form.replaceChildren(
    el("h2", { text: "Check the case" }),
    el("p", { class: "muted", text: "The AI built this from the clip and your description. Every advocate treats the " +
                                    "established facts as true, so fix anything that's wrong before the discussion starts." }),
    view.warnings.length ? el("ul", { class: "warnings" }, view.warnings.map((w) => el("li", { text: w }))) : null,
    el("label", { class: "field" }, "What happened", incident),
    el("h3", { text: "Possible rulings" }),
    rulingInputs.map(([id, input]) => el("label", { class: "field small" }, id, input)),
    el("label", { class: "field" }, "The officials' call", call),
    el("h3", { text: "Established facts" }),
    el("p", { class: "facts-note", text: "Everyone in the discussion treats these as true." }),
    facts.node,
    el("h3", { text: "Disputed points" }),
    disputed.node,
    el("h3", { text: "Parties, and the ruling each wants most" }),
    el("div", { class: "parties" }, parties.map(({ p, select }) => el("div", { class: "party" }, el("span", { text: p.name }), select))),
    el("h3", { text: "Rules the advocates may cite" }),
    el("ul", { class: "chips" }, c.rules.map((r) => el("li", { text: `${r}: ${titles[r] || ""}` }))),
    frames.length ? [el("h3", { text: "Frames from the clip" }), framesStrip(frames, [])] : null,
    el("details", { class: "options" },
      el("summary", { text: "Edit everything as JSON" }),
      el("p", { class: "hint", text: "If you change this, it's used instead of the fields above." }),
      json),
    el("div", { class: "actions" }, el("button", { class: "button primary", type: "submit", text: "Start the discussion" }), error),
  );
  form.hidden = false;
  form.onsubmit = async (event) => {
    event.preventDefault();
    error.textContent = "";
    let edited;
    try {
      edited = json.value !== original ? JSON.parse(json.value) : {
        incident: incident.value.trim() || c.incident,
        call: call.value,
        rulings: Object.fromEntries(rulingInputs.map(([id, input]) => [id, input.value.trim() || c.rulings[id]])),
        facts: numbered("F", facts.values()),
        disputed: numbered("D", disputed.values()),
        parties: parties.map(({ p, order, select }) => ({ ...p, likes: likesWithFirst(order, select.value) })),
      };
    } catch (err) {
      error.textContent = `That JSON doesn't parse: ${err.message}`;
      return;
    }
    try {
      await postJson(`/api/reviews/${review.id}/start`, { case: edited });
      form.hidden = true;
    } catch (err) {
      error.textContent = err.message;
    }
  };
  form.scrollIntoView({ behavior: "smooth", block: "start" });
}

// ---- the ruling ----
function voteCards(votes, notes = {}) {
  return el("div", { class: "votes" }, Object.entries(votes).map(([name, [vote, reason]]) => el("div", { class: "vote" },
    el("div", {}, el("span", { class: "who", text: name }), el("span", { class: "pick", text: vote || "no valid vote" })),
    reason ? el("p", { text: reason }) : null,
    notes[name] ? el("p", { class: "seen", text: `What it saw in the clip, before the discussion: ${notes[name]}` }) : null)));
}

function drawRuling(view) {
  clearTimeout(pollTimer);
  const r = view.result;
  const c = view.case;
  const byPanel = r.decided_by === "panel";
  const size = byPanel ? Object.keys(r.votes).length : Object.keys(r.positions).length;
  let how;
  if (r.status === "CONSENSUS") how = byPanel ? `The panel was unanimous (${size} of ${size}).` : "The advocates all came to agree.";
  else if (r.status === "VOTE") how = `${byPanel ? "The panel voted" : "The advocates voted"} ${r.tally[r.choice]} of ${size} for it.`;
  else how = `${byPanel ? "The panel" : "The advocates"} split (${Object.entries(r.tally).map(([k, v]) => `${k} ${v}`).join(", ") || "no valid votes"}), so ${r.how}.`;

  const box = $("ruling");
  box.replaceChildren(
    el("div", { class: `verdict ${r.upheld ? "upheld" : "overturned"}` },
      el("div", { class: "label", text: r.upheld ? "Call upheld" : "Call overturned" }),
      el("div", { class: "ruling", text: r.ruling }),
      el("p", { class: "how", text: `${how} After ${r.rounds} round${r.rounds > 1 ? "s" : ""} of discussion.` })),
    byPanel ? [el("h3", { text: "How the panel voted" }), voteCards(r.votes, r.notes)] : null,
    !byPanel && Object.keys(r.votes).length ? [el("h3", { text: "How the advocates voted" }), voteCards(r.votes)] : null,
    c.frame_urls.length ? [
      el("h3", { text: "Frames from the clip" }),
      byPanel ? el("p", { class: "hint", text: "Outlined: the frames the panel looked at." }) : null,
      framesStrip(c.frame_urls, r.frames_seen || []),
    ] : null,
    el("h3", { text: "The advocates" }),
    el("div", { class: "votes" }, Object.entries(r.positions).map(([name, position]) => el("div", { class: "vote" },
      el("div", {}, el("span", { class: "who", text: name }), el("span", { class: "pick", text: position })),
      r.statements[name] ? el("p", { text: r.statements[name] }) : null))),
    el("details", {}, el("summary", { text: "Full transcript" }), el("pre", { class: "log", text: $("log").textContent })),
    el("div", { class: "actions" }, againButton()),
  );
  $("progress").hidden = true;
  box.hidden = false;
  box.scrollIntoView({ behavior: "smooth", block: "start" });
}

function againButton() {
  return el("button", { type: "button", class: "button primary", text: "Review another call", onclick: () => {
    clearTimeout(pollTimer);
    for (const id of ["progress", "check-case", "ruling"]) $(id).hidden = true;
    $("ask").hidden = false;
    $("ask").scrollIntoView({ behavior: "smooth", block: "start" });
  } });
}

// ---- wiring ----
$("url").addEventListener("input", debounce(showLink, 400));
$("file").addEventListener("change", () => {
  file = $("file").files[0] || null;
  $("file-name").textContent = file ? file.name : "";
  if (file) {
    $("url").value = "";
    note("");
    showVideo(URL.createObjectURL(file));
  }
});
$("set-start").addEventListener("click", () => { if (player) $("start").value = clock(player.time()); });
$("set-end").addEventListener("click", () => { if (player) $("end").value = clock(player.time()); });
$("ask").addEventListener("submit", submitAsk);
api("/api/config").then((c) => { config = c; $("rounds").value = c.rounds; showLink(); }).catch(() => {});
