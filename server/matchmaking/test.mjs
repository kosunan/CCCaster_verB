import test from 'node:test';
import assert from 'node:assert/strict';
import { initialState, act } from './core.mjs';
function setup(mode='ranked') {
    let state=initialState(), serial=0, now=1000;
    const call=(player, route, body={}) => {
        const copy=structuredClone(state);
        const result=act(copy,player,route,body,now,()=>`match-${++serial}`);
        state=copy; return result;
    };
    const join=(p,m=mode)=>call(p,'/queue/join',{mode:m,protocol:10,code:'ABCDEFGH1234'});
    join('p_a'); const match=join('p_b').match;
    const report=(p,game,winner,selections)=>call(p,'/match/report',{id:match.id,game,winner,selections});
    return {call,join,match,report,advance(ms){now+=ms;}};
}
const picks=[{character:1,style:0},{character:2,style:1}];
test('1段開始、2試合先取、双方合意してから加点、再送で重複加点しない',()=>{
    const s=setup(); assert.equal(s.call('p_a','/me').dan,1);
    s.report('p_a',0,0,picks); assert.equal(s.call('p_a','/me').points,0);
    s.report('p_b',0,0,picks); assert.equal(s.call('p_a','/me').points,0);
    const changed=[picks[0],{character:2,style:2}];
    s.report('p_a',1,0,changed); const end=s.report('p_b',1,0,changed);
    assert.equal(end.finished,true); assert.deepEqual(end.scores,[2,0]);
    assert.equal(s.call('p_a','/me').points,100);
    s.report('p_b',1,0,changed); assert.equal(s.call('p_a','/me').points,100);
    assert.equal(s.call('p_b','/me').points,0);
});
test('勝者はキャラとスタイル固定、敗者はスタイルだけ変更可能',()=>{
    const s=setup(); s.report('p_a',0,0,picks);s.report('p_b',0,0,picks);
    assert.throws(()=>s.report('p_a',1,1,[{character:1,style:2},picks[1]]),/Winner style/);
    assert.throws(()=>s.report('p_b',1,1,[picks[0],{character:3,style:1}]),/Character/);
    const second=[picks[0],{character:2,style:2}];
    s.report('p_a',1,1,second);s.report('p_b',1,1,second);
    const third=[{character:1,style:1},second[1]];
    s.report('p_a',2,1,third);assert.equal(s.report('p_b',2,1,third).finished,true);
    assert.equal(s.call('p_b','/me').points,100);
});
test('結果不一致は段位を更新しない',()=>{
    const s=setup();s.report('p_a',0,0,picks);
    assert.equal(s.report('p_b',0,1,picks).disputed,true);
    assert.equal(s.call('p_a','/me').points,0);
});
test('カジュアルは1試合で終了、ポイント変動なし',()=>{
    const s=setup('casual');s.report('p_a',0,1,picks);
    assert.equal(s.report('p_b',0,1,picks).finished,true);
    assert.equal(s.call('p_b','/me').points,0);
});
test('キューはモード分離、重複参加防止、TTLで退出',()=>{
    let state=initialState();
    const join=(p,mode,t=1000)=>act(state,p,'/queue/join',{mode,protocol:10,code:'ABCDEFGH'},t,()=> 'm');
    assert.equal(join('p_a','casual').status,'waiting');
    assert.equal(join('p_b','ranked').status,'waiting');
    assert.throws(()=>join('p_a','ranked'),/Cancel/);
    assert.equal(act(state,'p_a','/queue/status',{},92000).status,'idle');
});
test('他人の対戦を閲覧・報告できない',()=>{
    const s=setup();assert.throws(()=>s.call('p_c','/match',{id:s.match.id}),/not found/);
});
test('段位ポイント300で2段、セット期限切れは加点しない',()=>{
    const state=initialState();state.profiles.p_a={points:300};
    assert.equal(act(state,'p_a','/me',{},1000).dan,2);
    const s=setup();s.advance(30*60_000+1);
    assert.equal(s.call('p_a','/match',{id:s.match.id}).aborted,true);
    assert.equal(s.call('p_a','/me').points,0);
});

test('組合せ確定とキャンセルが競合しても決定済みの相手を失わない',()=>{
    const s=setup();
    const result=s.call('p_a','/queue/cancel');
    assert.equal(result.status,'matched'); assert.equal(result.match.id,s.match.id);
});
