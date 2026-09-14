import { DurableObject } from 'cloudflare:workers';
import { initialState, act } from './core.mjs';
const reply = (body, status = 200) => Response.json(body, {status, headers:{'Cache-Control':'no-store'}});
export default {
    async fetch(request, env) {
        // 試験は招待アカウント制。SHA-256(token) → player ID をsecretに登録する。
        if (!env.PLAYERS_JSON) return reply({error:'Server not configured'}, 503);
        if (request.method !== 'POST') return reply({error:'POST required'}, 405);
        const token = request.headers.get('Authorization')?.match(/^Bearer ([A-Za-z0-9_-]{32,128})$/)?.[1];
        if (!token) return reply({error:'Authentication required'}, 401);
        const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(token));
        const hash = Array.from(new Uint8Array(digest), b => b.toString(16).padStart(2,'0')).join('');
        let players;
        try { players = JSON.parse(env.PLAYERS_JSON); } catch { return reply({error:'Server not configured'},503); }
        if (!players || typeof players !== 'object' || Array.isArray(players)) return reply({error:'Server not configured'},503);
        const player = Object.hasOwn(players, hash) ? players[hash] : null;
        if (typeof player !== 'string' || !/^p_[a-zA-Z0-9_-]{1,60}$/.test(player)) return reply({error:'Invalid account'},401);
        const text = await request.text();
        if (text.length > 4096) return reply({error:'Request too large'},413);
        const headers = new Headers({'Content-Type':'application/json','X-Player':player});
        const id = env.LOBBY.idFromName('trial-v1');
        return env.LOBBY.get(id).fetch(new Request(request.url, {method:'POST', headers, body:text}));
    }
};
export class Lobby extends DurableObject {
    async fetch(request) {
        return this.ctx.blockConcurrencyWhile(async () => {
            try {
                const body = await request.json();
                if (!body || typeof body !== 'object' || Array.isArray(body)) return reply({error:'Invalid body'},400);
                const state = await this.ctx.storage.get('state') ?? initialState();
                const result = act(state, request.headers.get('X-Player'), new URL(request.url).pathname, body);
                await this.ctx.storage.put('state', state);
                return reply(result);
            } catch (error) {
                return reply({error: error.status ? error.message : 'Invalid request'}, error.status ?? 400);
            }
        });
    }
}
