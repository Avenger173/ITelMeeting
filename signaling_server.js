const WebSocket=require('ws');
const wss=new WebSocket.Server({port: 7000});
const rooms=new Map();//roomId->Set<ws>

wss.on('connection',ws=>{
    ws.on('message',msg=>{
        const {type,room,payload}=JSON.parse(msg);
        if(type==='join'){
            if(!rooms.has(room)) rooms.set(room,new Set());
            rooms.get(room).add(ws);
            ws._room=room;
        }else if(type==='signal'){
            if(!ws._room)   return;
            for(const peer of rooms.get(ws._room)){
                if(peer!==ws) peer.send(JSON.stringify({type:'signal',payload}));
            }
        }
    });
    ws.on('close',()=>{
        if(ws._room&&rooms.has(ws._room)) rooms.get(ws._room).delete(ws);
    });
});
console.log('Signaling server on ws://localhost:7000');