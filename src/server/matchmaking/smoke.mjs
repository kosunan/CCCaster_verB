import { Miniflare, convertV4MiniflareOptions } from 'miniflare';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
// ローカル試験専用の使い捨てトークン。公開サーバーには登録しない。
const tokens=['a'.repeat(40),'b'.repeat(40)];
const players=Object.fromEntries(tokens.map((t,i)=>[createHash('sha256').update(t).digest('hex'),`p_${i}`]));
const mf=new Miniflare(convertV4MiniflareOptions({modules:[{type:'ESModule',path:'worker.mjs'},{type:'ESModule',path:'core.mjs'}],
    compatibilityDate:'2025-04-01', durableObjects:{LOBBY:{className:'Lobby',useSQLite:true}},
    bindings:{PLAYERS_JSON:JSON.stringify(players)}}));
const deadline=setTimeout(()=>{console.error('Local runtime timed out after 30 seconds');process.exit(1);},30000);

async function request(path,token,body={}) {
    return mf.dispatchFetch('https://local.test'+path,{method:'POST',
        headers:{'Authorization':'Bearer '+token,'Content-Type':'application/json'},body:JSON.stringify(body)});
}
try {
    assert.equal((await request('/me','invalid')).status,401);
    assert.equal((await (await request('/me',tokens[0])).json()).dan,1);
    const body={mode:'ranked',protocol:10,code:'ABCDEFGH1234'};
    assert.equal((await (await request('/queue/join',tokens[0],body)).json()).status,'waiting');
    const result=await (await request('/queue/join',tokens[1],body)).json();
    assert.equal(result.status,'matched');
    assert.equal((await (await request('/queue/status',tokens[0])).json()).match.id,result.match.id);
    const selections=[{character:1,style:0},{character:2,style:1}];
    for(let game=0;game<2;game++) for(const token of tokens) {
        const response=await request('/match/report',token,{id:result.match.id,game,winner:0,selections});
        assert.equal(response.status,200,await response.text());
    }
    assert.equal((await (await request('/me',tokens[0])).json()).points,100);
    console.log('HTTP認証・待機列・組合せ・双方報告・加点: 成功');
} finally { clearTimeout(deadline); await mf.dispose(); }
