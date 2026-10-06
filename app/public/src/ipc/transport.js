/**
 * transport.js — El puente de este port.
 *
 * Sustituye a `window.__TAURI__` con tres piezas:
 *
 * 1. `invocar(cmd, args)`: HTTP loopback contra `/api/invoke`, el despachador
 *    que habla el dialecto de comandos de Localify. Tres comandos NO viajan:
 *    `player_position` y `player_get_state` —la posición la dueña el <audio>
 *    de este documento, y devolver la del servidor pintaría un tiempo
 *    viejo en la barra— y los diálogos nativos (`window.open_file` de Lux).
 *
 * 2. El motor de audio: un elemento <audio> único que sigue al estado del
 *    backend. En Localify el sonido salía del motor Rust; aquí lo decodifica
 *    GStreamer dentro del WebView, y el backend sigue siendo la fuente de
 *    verdad de QUÉ suena.
 *
 *    ## El ecualizador va aparte y se monta solo si hace falta
 *
 *    Enrutar el elemento por Web Audio (MediaElementAudioSourceNode) es
 *    irreversible para el elemento y en WebKitGTK ha dado audio distorsionado
 *    —el "chirrido" al empezar a sonar—. Por eso el elemento suena DIRECTO
 *    por defecto, y la cadena de 10 biquads solo se monta cuando el usuario
 *    aplica un perfil que no es el plano.
 *
 * 3. El bombeo de eventos: sondea `/api/events/poll` cada segundo y
 *    sintetiza los eventos del bus de Localify con sus nombres exactos,
 *    comparando el snapshot anterior y el nuevo.
 */

const audio = new Audio();
audio.id = "audio";
document.body.append(audio);

// ── Ecualizador de 10 bandas (Web Audio, bajo demanda) ──────────────────────

const BANDAS = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000];
let cadenaEq = null;
let gananciasEq = new Float32Array(10);

function eqNoEsPlano() {
	for (let i = 0; i < 10; i++) {
		if (Math.abs(gananciasEq[i] ?? 0) > 0.01) return true;
	}
	return false;
}

/** Monta la cadena SOLO cuando hay ganancias que aplicar. */
function montarEqSiHaceFalta() {
	if (cadenaEq || !eqNoEsPlano()) return;
	try {
		const ctx = new AudioContext();
		const fuente = ctx.createMediaElementSource(audio);
		let nodo = fuente;
		const filtros = BANDAS.map((hz) => {
			const f = ctx.createBiquadFilter();
			f.type = "peaking";
			f.frequency.value = hz;
			f.Q.value = 1.41;
			f.gain.value = 0;
			nodo.connect(f);
			nodo = f;
			return f;
		});
		nodo.connect(ctx.destination);
		cadenaEq = { ctx, filtros };
		aplicarGanancias();
		if (ctx.state === "suspended") ctx.resume().catch(() => {});
	} catch {
		// sin Web Audio: suena sin EQ (el elemento sigue directo)
		cadenaEq = null;
	}
}

function aplicarGanancias() {
	if (!cadenaEq) {
		montarEqSiHaceFalta();
		if (!cadenaEq) return;
	}
	cadenaEq.filtros.forEach((f, i) => {
		f.gain.value = gananciasEq[i] ?? 0;
	});
}

function aplicarVolumenPerceptual(v) {
	// El oído responde de forma logarítmica: v³ es la curva del motor original.
	audio.volume = Math.min(1, Math.max(0, v)) ** 3;
}

// ── Estado local sincronizado ───────────────────────────────────────────────

let ultimoEstado = null;
let ultimaPistaCargada = "";
let pendienteRestaurarMs = 0; // posición a restaurar cuando carguen los metadatos

audio.addEventListener("loadedmetadata", () => {
	if (pendienteRestaurarMs > 3000 && isFinite(audio.duration) && audio.duration > 0) {
		try {
			audio.currentTime = Math.min(pendienteRestaurarMs / 1000, audio.duration - 1);
		} catch {}
	}
	pendienteRestaurarMs = 0;
});

audio.addEventListener("ended", () => {
	// Fin natural: el backend decide (repeat track/queue, historial…). Era el
	// AdvanceReason::NaturalEnd del PlaybackActor. La respuesta ya trae el
	// estado nuevo: se aplica al acto para que la siguiente pista cargue
	// sin esperar al siguiente sondeo.
	const ms = Math.round((audio.duration || 0) * 1000);
	fetch("/api/player/ended", {
		method: "POST",
		headers: { "Content-Type": "application/json" },
		body: JSON.stringify({ msPlayed: ms }),
	})
		.then((r) => (r.ok ? r.json() : null))
		.then((estado) => {
			if (estado && estado.track !== undefined) sincronizarDesde(estado);
		})
		.catch(() => {});
});

/**
 * Carga en el <audio> la pista que el backend dice sonar.
 * `estado` es el estado completo: la posición persistida solo se restaura
 * cuando la pista es distinta de la que estaba cargada (recuperar sesión),
 * nunca en un avance normal.
 */
function sincronizarPista(track, estado) {
	const id = track?.id ?? "";
	if (id === ultimaPistaCargada) {
		// misma pista: ¿repetición en modo "track"? el elemento terminó y el
		// backend la vuelve a poner: rebobinar y seguir
		if (estado && estado.status === "playing" && audio.ended) {
			try {
				audio.currentTime = 0;
				audio.play().catch(() => {});
			} catch {}
		}
		return;
	}
	ultimaPistaCargada = id;
	const disp = track?.availability;
	if (disp && disp.kind === "local") {
		pendienteRestaurarMs = estado?.positionMs ?? 0;
		audio.src = `/audio/${id}`;
		audio.load();
	} else {
		// descarga en curso: sin fuente todavía; cuando la disponibilidad
		// cambie a "local", el siguiente sondeo la carga
		pendienteRestaurarMs = 0;
		audio.removeAttribute("src");
		audio.load();
	}
}

/** Aplica el estado del backend al <audio>: qué suena, play/pause, volumen. */
function sincronizarDesde(estado) {
	if (!estado) return;
	ultimoEstado = estado;
	aplicarVolumenPerceptual(estado.volume ?? 1);
	sincronizarPista(estado.track, estado);
	const local = estado.track?.availability?.kind === "local";
	const quiere = estado.status === "playing";
	if (local && quiere) {
		if (audio.paused) {
			audio.play().catch(() => {});
		}
	} else if ((!local && !audio.paused) || (!quiere && !audio.paused)) {
		audio.pause();
	}
}

// ── Bombeo de eventos ───────────────────────────────────────────────────────

let alEvento = null;
let alResync = null;
let bombear = false;

function emitir(type, extra = {}) {
	if (!alEvento) return;
	alEvento({ type, ...extra });
}

/** Compara dos snapshots y emite los eventos del bus original que correspondan. */
function diferir(antes, ahora) {
	if (!antes) return;
	if (ahora.revision !== antes.revision) emitir("queueChanged", { revision: ahora.revision });
	if (ahora.trackId !== antes.trackId) emitir("trackChanged", { trackId: ahora.trackId, source: ahora.source ?? "queue" });
	if (ahora.status !== antes.status) emitir("playStatusChanged", { status: ahora.status });
	if (ahora.volume !== antes.volume) emitir("volumeChanged", { volume: ahora.volume });
	if (ahora.repeat !== antes.repeat) emitir("repeatModeChanged", { mode: ahora.repeat });
	if (ahora.shuffle !== antes.shuffle) emitir("shuffleChanged", { enabled: ahora.shuffle });

	// descargas: progreso, completado (→ disponibilidad) y fallo
	const previas = new Map(antes.downloads.map((d) => [d.trackId, d]));
	const vistas = new Set();
	for (const d of ahora.downloads) {
		vistas.add(d.trackId);
		const p = previas.get(d.trackId);
		if (d.state === "downloading") {
			const total = d.bytesTotal > 0 ? d.bytesTotal : 0;
			const pct = total > 0 ? Math.min(100, Math.round((d.bytesDone / total) * 100)) : 0;
			// limitado a 2 Hz por descarga, como en el emisor original
			const ahora_ = Date.now();
			const clave = "throttle:" + d.trackId;
			const ultima = moduleState.get(clave) ?? 0;
			if (ahora_ - ultima >= 500) {
				moduleState.set(clave, ahora_);
				emitir("downloadProgress", { trackId: d.trackId, percent: pct });
			}
		}
		if (p && p.state !== "failed" && d.state === "failed") {
			emitir("downloadFailed", { trackId: d.trackId, reasonKey: d.lastError ?? "download.failed", attempts: d.attempts });
		}
	}
	for (const [tid, p] of previas) {
		if (!vistas.has(tid) && p.state !== "failed") {
			// el trabajo desapareció: o terminó bien (la fila se borra al
			// finalizar) o lo borró un wipe
			emitir("downloadCompleted", { trackId: tid });
			emitir("availabilityChanged", {
				trackId: tid,
				availability: { kind: "local", format: "opus", bytes: 0 },
			});
		}
	}

	if (ahora.libraryStamp !== antes.libraryStamp) emitir("libraryChanged", {});
	if (ahora.playlistStamp !== antes.playlistStamp) emitir("playlistChanged", {});
	if (ahora.settingsStamp !== antes.settingsStamp) emitir("settingsChanged", {});
	if (ahora.libraryPath !== antes.libraryPath) emitir("libraryPathChanged", { path: ahora.libraryPath });
	for (const r of ahora.remoteReady ?? []) {
		emitir("searchRemoteReady", { queryId: r.queryId });
	}
}

const moduleState = new Map();
let pollEnMarcha = false;

async function tick() {
	if (pollEnMarcha) return;
	pollEnMarcha = true;
	try {
		const r = await fetch("/api/events/poll", { method: "POST" });
		const snap = await r.json();
		if (snap.estado) {
			// el <audio> manda sobre la posición visible: el valor del servidor
			// se persiste cada pocos segundos y pintarlo directamente haría que
			// la barra se moviera a saltos o se quedara quieta
			if (snap.estado.track && snap.estado.track.id === ultimaPistaCargada && !audio.paused) {
				snap.estado.positionMs = Math.round(audio.currentTime * 1000);
			}
			ultimoEstado = snap.estado;
		}
		sincronizarDesde(ultimoEstado);
		diferir(moduleState.get("prev") ?? null, snap);
		moduleState.set("prev", snap);
	} catch {
		// sin servidor (reinicio de la app): el siguiente tick reintenta
	}
	pollEnMarcha = false;
}

export function arrancarEventos(alEventoCb, alResyncCb) {
	alEvento = alEventoCb;
	alResync = alResyncCb;
	if (bombear) return;
	bombear = true;
	setInterval(tick, 1000);
	tick();
	iniciarEqGuardado();
}

// ── Invocación ──────────────────────────────────────────────────────────────

/** Diálogos nativos: los abre la ventana GTK (módulo window de Lux). */
async function dialogoAbrir() {
	const r = await fetch("/api/native/open-file", { method: "POST" });
	if (!r.ok) return "";
	const j = await r.json();
	return j.path ?? "";
}

/**
 * Invoca un comando. La posición de reproducción y los diálogos no viajan:
 * la primera vive en este documento, los segundos en la ventana nativa.
 */
export async function invocar(cmd, args) {
	// ── comandos que se responden en el cliente ──
	if (cmd === "player_position") {
		const dur = isFinite(audio.duration) ? audio.duration * 1000 : 0;
		const pos = audio.currentTime * 1000;
		return {
			positionMs: Math.round(pos),
			bufferedMs: Math.round(dur > 0 ? dur : pos),
		};
	}
	if (cmd === "library_pick_import_files" || cmd === "settings_pick_cookies") {
		const ruta = await dialogoAbrir();
		return ruta ? [ruta] : [];
	}
	if (cmd === "settings_pick_folder") {
		const ruta = await dialogoAbrir();
		return ruta || null;
	}
	if (cmd === "playlist_pick_image") {
		const ruta = await dialogoAbrir();
		return ruta || null;
	}

	// la posición local viaja como metadato para la regla de los 3 s y el
	// historial: el backend del original la conocía de primera mano
	const conPos = { ...args };
	if (["player_toggle", "player_pause", "player_next", "player_previous"].includes(cmd)) {
		conPos.__posMs = Math.round((audio.currentTime || 0) * 1000);
	}

	const r = await fetch("/api/invoke", {
		method: "POST",
		headers: { "Content-Type": "application/json" },
		body: JSON.stringify({ cmd, args: JSON.stringify(conPos) }),
	});
	const j = await r.json().catch(() => null);
	if (!r.ok) {
		throw j ?? { code: "INTERNAL", messageKey: "error.internal", params: [], actionable: false, retryable: false };
	}

	// ── efectos locales inmediatos, para que la barra no espere al sondeo ──
	if (cmd === "player_seek" && args && typeof args.positionMs === "number" && isFinite(audio.duration) && audio.duration > 0) {
		try {
			audio.currentTime = Math.min(args.positionMs / 1000, audio.duration);
		} catch {}
	} else if (cmd === "player_set_volume" && args && typeof args.volume === "number") {
		aplicarVolumenPerceptual(args.volume);
	} else if (cmd === "player_toggle") {
		// el backend ya alternó: el estado devuelto manda
		sincronizarDesde(j);
	} else if (cmd === "player_pause") {
		sincronizarDesde(j ?? { ...ultimoEstado, status: "paused" });
	} else if (cmd === "player_resume" || cmd === "player_play_track" || cmd === "player_next" || cmd === "player_previous" || cmd === "queue_jump_to") {
		if (j && j.track !== undefined) sincronizarDesde(j);
		else if (ultimoEstado?.track?.availability?.kind === "local") audio.play().catch(() => {});
	}
	if (cmd === "settings_patch" || cmd === "settings_preview_eq") {
		const perfil = args?.patch?.audio?.eqProfile ?? args?.profile;
		if (perfil && Array.isArray(perfil.gainsDb)) {
			gananciasEq = new Float32Array(perfil.gainsDb);
			aplicarGanancias();
		}
	}
	return j;
}

/** Arranque: aplica el EQ guardado en cuanto haya respuesta de ajustes. */
export async function iniciarEqGuardado() {
	try {
		const r = await fetch("/api/invoke", {
			method: "POST",
			headers: { "Content-Type": "application/json" },
			body: JSON.stringify({ cmd: "settings_get", args: JSON.stringify({}) }),
		});
		const s = await r.json();
		const ganancias = s?.audio?.eqProfile?.gainsDb;
		if (Array.isArray(ganancias)) {
			gananciasEq = new Float32Array(ganancias);
			if (eqNoEsPlano()) {
				aplicarGanancias();
			}
		}
	} catch {}
}

// ── Diagnóstico: los fallos de la UI llegan al log del backend ──────────────
// Sin devtools en el empaquetado, un error de consola era invisible. Cada
// error de script o promesa rechazada se reporta a /api/client-log y queda
// en el log del servidor con su stack.

function reportar(tipo, detalle) {
	try {
		fetch("/api/client-log", {
			method: "POST",
			headers: { "Content-Type": "application/json" },
			body: JSON.stringify({ tipo, detalle: String(detalle).slice(0, 2000) }),
		}).catch(() => {});
	} catch {}
}

window.addEventListener("error", (e) => {
	reportar("error", `${e.message} @ ${e.filename}:${e.lineno}:${e.colno}`);
});
window.addEventListener("unhandledrejection", (e) => {
	reportar("promesa", e.reason && e.reason.stack ? e.reason.stack : String(e.reason));
});
