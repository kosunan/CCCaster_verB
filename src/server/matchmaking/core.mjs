// 初回試験用の段位ポイント。サービス開始前に合意して設定する。
export const RULES = Object.freeze({ winsToFinish: 2, winPoints: 100, promotionPoints: 300 });
const QUEUE_TTL = 90_000;
const MATCH_TTL = 30 * 60_000;
export const initialState = () => ({ profiles: {}, queue: {}, matches: {} });
const fail = (message, status = 400) => { throw Object.assign(new Error(message), { status }); };
const requireValue = (value, message) => { if (!value) fail(message); };
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
export function profile(state, id) {
    // IDは認証済みのサーバー設定からのみ渡す。
    state.profiles[id] ??= { points: 0 };
    const points = state.profiles[id].points;
    return { id, points, dan: 1 + Math.floor(points / RULES.promotionPoints),
             nextPromotionPoints: RULES.promotionPoints - points % RULES.promotionPoints };
}
function expire(state, now) {
    for (const [id, entry] of Object.entries(state.queue))
        if (now - entry.touched > QUEUE_TTL) delete state.queue[id];
    for (const match of Object.values(state.matches))
        if (!match.finished && now - match.created > MATCH_TTL) {
            match.finished = true; match.aborted = true;
        }
    // 確定結果は1日保持して再送に対応。段位はprofile側に残す。
    for (const [id, match] of Object.entries(state.matches))
        if (now - match.created > 86_400_000) delete state.matches[id];
}
function activeMatch(state, player) {
    return Object.values(state.matches).find(m => !m.finished && m.players.includes(player));
}
function matchFor(state, player, id) {
    const match = state.matches[id];
    if (!match || !match.players.includes(player)) fail('Match not found', 404);
    return match;
}
function view(match, player) {
    return { id: match.id, mode: match.mode, players: match.players, scores: match.scores,
             game: match.games.length, host: match.players[0],
             hostCode: match.hostCode, finished: match.finished,
             aborted: !!match.aborted, disputed: !!match.disputed,
             lastGame: match.games.at(-1) ?? null,
             reportPending: !!match.pending[player] };
}
function validateGame(match, game) {
    requireValue(Number.isInteger(game.winner) && (game.winner === 0 || game.winner === 1), 'Invalid winner');
    requireValue(Array.isArray(game.selections) && game.selections.length === 2, 'Two selections required');
    const selections = game.selections.map(s => {
        requireValue(Number.isInteger(s.character) && s.character >= 0 && s.character < 256, 'Invalid character');
        requireValue(Number.isInteger(s.style) && s.style >= 0 && s.style <= 2, 'Invalid style');
        return { character: s.character, style: s.style };
    });
    if (match.mode === 'ranked' && match.games.length) {
        const previous = match.games.at(-1);
        for (let i = 0; i < 2; i++) {
            requireValue(selections[i].character === previous.selections[i].character, 'Character is locked');
            if (i === previous.winner)
                requireValue(selections[i].style === previous.selections[i].style, 'Winner style is locked');
        }
    }
    return { winner: game.winner, selections };
}
// 更新は呼び出し側がトランザクション内のコピーに対して適用し、成功時のみ保存する。
export function act(state, player, route, body = {}, now = Date.now(), makeId = () => crypto.randomUUID()) {
    expire(state, now);
    profile(state, player);
    if (route === '/me') return profile(state, player);
    if (route === '/queue/status') {
        if (state.queue[player]) state.queue[player].touched = now;
        const match = activeMatch(state, player);
        return match ? { status: 'matched', match: view(match, player) }
                     : { status: state.queue[player] ? 'waiting' : 'idle' };
    }
    if (route === '/queue/cancel') {
        const match = activeMatch(state, player);
        if (match) return {status:'matched', match:view(match,player)};
        delete state.queue[player]; return {status:'idle'};
    }
    if (route === '/queue/join') {
        requireValue(['ranked', 'casual'].includes(body.mode), 'Invalid mode');
        requireValue(body.protocol === 10, 'Incompatible protocol');
        requireValue(typeof body.code === 'string' && /^[A-Za-z0-9]{8,512}$/.test(body.code), 'Invalid connection code');
        const current = activeMatch(state, player);
        if (current) return { status: 'matched', match: view(current, player) };
        const existing = state.queue[player];
        if (existing && existing.mode !== body.mode) fail('Cancel before switching modes', 409);
        state.queue[player] = { mode: body.mode, code: body.code, touched: now, joined: existing?.joined ?? now };
        const candidates = Object.entries(state.queue).filter(([id, q]) => id !== player && q.mode === body.mode);
        candidates.sort(([a, qa], [b, qb]) => body.mode === 'ranked'
            ? Math.abs(profile(state,a).dan - profile(state,player).dan) - Math.abs(profile(state,b).dan - profile(state,player).dan) || qa.joined - qb.joined
            : qa.joined - qb.joined);
        if (!candidates.length) return { status: 'waiting' };
        const [opponent, entry] = candidates[0];
        const id = makeId();
        const match = { id, mode: body.mode, players: [opponent, player], hostCode: entry.code,
            scores: [0,0], games: [], pending: {}, created: now, finished: false };
        state.matches[id] = match;
        delete state.queue[player]; delete state.queue[opponent];
        return { status: 'matched', match: view(match, player) };
    }
    if (route === '/match') return view(matchFor(state, player, body.id), player);
    if (route === '/match/report') {
        const match = matchFor(state, player, body.id);
        requireValue(Number.isInteger(body.game) && body.game >= 0, 'Invalid game index');
        // 再送済みの確定試合は同一内容の場合だけ許可し、加点し直さない。
        if (body.game < match.games.length) {
            const stored = match.games[body.game];
            requireValue(same({winner:body.winner, selections:body.selections}, stored), 'Conflicting report');
            return view(match, player);
        }
        if (match.finished) fail('Match is closed', 409);
        requireValue(body.game === match.games.length, 'Unexpected game index');
        const game = validateGame(match, body);
        if (match.pending[player] && !same(match.pending[player], game)) fail('Cannot change a submitted result', 409);
        match.pending[player] = game;
        const [a,b] = match.players.map(id => match.pending[id]);
        if (a && b) {
            if (!same(a,b)) { match.finished = true; match.disputed = true; }
            else {
                match.games.push(a); match.pending = {};
                match.scores[a.winner]++;
                const target = match.mode === 'ranked' ? RULES.winsToFinish : 1;
                if (match.scores[a.winner] === target) {
                    match.finished = true;
                    if (match.mode === 'ranked') state.profiles[match.players[a.winner]].points += RULES.winPoints;
                }
            }
        }
        return view(match, player);
    }
    fail('Not found', 404);
}
