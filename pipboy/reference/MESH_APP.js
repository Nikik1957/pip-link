(function(params){
  // ---- MeshCore Viewer (simulated data) ----
  var view = "list";        // list | chat | compose
  var filter = "ch";        // list filter: ch | dm | all  (default: channels)
  var sel = 0;
  var active = null;
  var chatScroll = 0;       // -1 = pin to bottom
  var rxFlash = 0;
  var timers = [];

  var convos = [];   // starts empty; the bridge seeds real contacts/channels

  var NODES = ["K7RAD","VAULT-101","MOBILE-6","REPEATER-9","BROTHERHOOD","CARAVAN-2"];
  var CHATTER = ["QSL, 73","anyone around?","rad levels nominal","ping","net in 10 min",
    "position update sent","copy that","signal weak here","caravan inbound","monitoring",
    "good to hear you","weather moving in","battery at 80%","relaying your msg"];

  // ---- keyboard model ----
  var KROWS = [
    ["1","2","3","4","5","6","7","8","9","0"],
    ["Q","W","E","R","T","Y","U","I","O","P"],
    ["A","S","D","F","G","H","J","K","L","/"],
    ["Z","X","C","V","B","N","M",".",",","?"],
    ["SP","DEL","-","SEND","CANC"]
  ];
  var kb = { r:1, c:0, buf:"", blink:true };

  function strengthFromSnr(s){ return s>=10?4 : s>=5?3 : s>=0?2 : s>=-6?1 : 0; }
  function totalMsgs(){ var n=0; for(var i=0;i<convos.length;i++) n+=convos[i].msgs.length; return n; }
  function chatVis(){ return Math.floor((316 - 24 - 36) / 20); }
  // list filtering: channels by default, nodes/all on knob2 toggle
  function visible(){
    if(filter === "all") return convos;
    var out = [];
    for(var i=0;i<convos.length;i++) if(convos[i].kind === filter) out.push(convos[i]);
    return out;
  }
  // move a convo to the top on new activity, keeping the highlighted convo under the cursor
  function bump(c){
    var vis = visible(), selConvo = vis[sel];
    var i = convos.indexOf(c);
    if(i > 0){ convos.splice(i, 1); convos.unshift(c); }
    var nv = visible(), ns = nv.indexOf(selConvo);
    sel = ns >= 0 ? ns : 0;
  }

  function drawBars(x, y, strg){
    for(var k=0;k<4;k++){
      h.setColor(k<strg ? 3 : 1);
      var bh = 4 + k*3;
      h.fillRect(x + k*6, y + (12-bh), x + k*6 + 4, y + 12);
    }
  }
  function wrapText(str, maxW){
    var words = str.split(" "), lines = [], cur = "";
    for(var i=0;i<words.length;i++){
      var test = cur ? cur + " " + words[i] : words[i];
      if(h.stringWidth(test) > maxW && cur){ lines.push(cur); cur = words[i]; }
      else cur = test;
    }
    if(cur) lines.push(cur);
    return lines;
  }

  function handleIncoming(convoName, from, text, snr){
    var c = null;
    for(var i=0;i<convos.length;i++) if(convos[i].name === convoName) c = convos[i];
    if(!c){ c = { name:convoName, kind:"dm", snr:snr, unread:0, msgs:[] }; convos.push(c); }
    c.msgs.push({ dir:"in", from:from, text:text, snr:snr });
    if(c.msgs.length > 40) c.msgs = c.msgs.slice(-40);
    c.snr = snr;
    rxFlash = 3;
    try { Pip.playSound("SCROLL"); } catch(e){}
    if((view === "chat" || view === "compose") && active === c) chatScroll = -1;
    else c.unread++;
    bump(c);
    draw();
  }
  function simulate(){
    if(!convos.length) return;
    var c = convos[Math.floor(Math.random()*convos.length)];
    var from = c.kind === "dm" ? c.name : NODES[Math.floor(Math.random()*NODES.length)];
    var text = CHATTER[Math.floor(Math.random()*CHATTER.length)];
    var snr = Math.round((Math.random()*20 - 6) * 10) / 10;
    handleIncoming(c.name, from, text, snr);
  }
  function sendMsg(c, text){
    c.msgs.push({ dir:"out", from:"you", text:text, snr:0 });
    if(c.msgs.length > 40) c.msgs = c.msgs.slice(-40);
    chatScroll = -1;
    bump(c);
    try { print("<<MESH-TX " + btoa(c.name) + " " + btoa(text) + ">>"); } catch(e){}
  }

  // ---- shared chrome ----
  function header(txt){
    h.setColor(2); h.setFont("Monofonto16"); h.setFontAlign(-1,0);
    h.drawString(txt, 14, 14);
    if(rxFlash > 0){ h.setColor(3); h.setFontAlign(1,0); h.drawString("RX", 466, 14); }
    h.setColor(1); h.drawLine(0, 28, 480, 28);
  }
  function footer(txt){
    h.setColor(1); h.setFont("Monofonto14"); h.setFontAlign(0,1);
    h.drawString(txt, 240, 316);
  }

  function drawList(){
    var vis = visible();
    var flabel = filter === "ch" ? "CHANNELS" : filter === "dm" ? "NODES" : "ALL";
    header("MESHCORE  [" + flabel + "]  " + vis.length);
    if(vis.length === 0){
      h.setColor(1); h.setFont("Monofonto18"); h.setFontAlign(0,0);
      h.drawString(filter === "dm" ? "no nodes yet..." : filter === "ch" ? "no channels yet..." : "waiting for mesh traffic...", 240, 160);
      footer("knob2: switch CHANNELS / NODES / ALL");
      return;
    }
    if(sel >= vis.length) sel = vis.length - 1;
    var rowH = 44, top = 36, nvis = 5;
    var start = Math.max(0, Math.min(sel - 2, vis.length - nvis));
    for(var i=0;i<nvis;i++){
      var idx = start + i; if(idx >= vis.length) break;
      var c = vis[idx], y = top + i*rowH;
      if(idx === sel){ h.setColor(1); h.fillRect(6, y, 474, y + rowH - 4); }
      h.setColor(idx === sel ? 3 : 2); h.setFont("Monofonto18"); h.setFontAlign(-1,0);
      h.drawString(c.name, 16, y + 14);
      drawBars(430, y + 8, strengthFromSnr(c.snr));
      if(c.unread > 0){ h.setColor(3); h.setFontAlign(1,0); h.drawString("(" + c.unread + ")", 412, y + 14); }
      var last = c.msgs[c.msgs.length - 1];
      if(last){
        h.setColor(1); h.setFont("Monofonto14"); h.setFontAlign(-1,0);
        var pv = (last.dir === "out" ? "you: " : last.from + ": ") + last.text;
        while(h.stringWidth(pv) > 452 && pv.length > 4) pv = pv.slice(0, -2);
        h.drawString(pv, 16, y + 32);
      }
    }
    footer("knob1: scroll  click: open  knob2: " + flabel);
  }

  function buildChatLines(c){
    var out = [];
    h.setFont("Monofonto16");
    for(var i=0;i<c.msgs.length;i++){
      var m = c.msgs[i];
      out.push({ head:true, dir:m.dir, text: m.dir === "in" ? (m.from + "  snr " + m.snr) : "you" });
      var body = wrapText(m.text, 448);
      for(var j=0;j<body.length;j++) out.push({ head:false, dir:m.dir, text:body[j] });
    }
    return out;
  }
  function drawChat(){
    var c = active;
    header(c.name + "   snr " + c.snr);
    var lines = buildChatLines(c), lineH = 20, top = 36, vis = chatVis();
    var maxStart = Math.max(0, lines.length - vis);
    var start = (chatScroll < 0) ? maxStart : Math.max(0, Math.min(chatScroll, maxStart));
    for(var i=0;i<vis;i++){
      var idx = start + i; if(idx >= lines.length) break;
      var ln = lines[idx], y = top + i*lineH;
      h.setFontAlign(ln.dir === "out" ? 1 : -1, 0);
      if(ln.head){ h.setColor(1); h.setFont("Monofonto14"); }
      else { h.setColor(ln.dir === "out" ? 3 : 2); h.setFont("Monofonto16"); }
      h.drawString(ln.text, ln.dir === "out" ? 466 : 14, y);
    }
    footer("knob1: scroll   click: reply   knob2: back");
  }

  function keyLabel(tok){ return tok === "SP" ? "SPC" : tok; }
  function drawCompose(){
    h.setColor(2); h.setFont("Monofonto16"); h.setFontAlign(-1,0);
    h.drawString("MSG to " + active.name, 14, 14);
    if(rxFlash > 0){ h.setColor(3); h.setFontAlign(1,0); h.drawString("RX", 466, 14); }
    h.setColor(1); h.drawRect(14, 30, 466, 70);
    var shown = kb.buf + (kb.blink ? "_" : " ");
    h.setColor(3); h.setFont("Monofonto23"); h.setFontAlign(-1,0);
    while(h.stringWidth(shown) > 440 && shown.length > 1) shown = shown.slice(1);
    h.drawString(shown, 22, 50);
    var y0 = 98, rowH = 40;
    for(var r=0;r<KROWS.length;r++){
      var row = KROWS[r], special = (r === 4);
      var keyW = special ? 90 : 42, startX = 240 - (row.length*keyW)/2, y = y0 + r*rowH;
      h.setFont(special ? "Monofonto16" : "Monofonto18"); h.setFontAlign(0,0);
      for(var c=0;c<row.length;c++){
        var cx = startX + c*keyW + keyW/2, srsel = (r === kb.r && c === kb.c);
        if(srsel){ h.setColor(1); h.fillRect(cx - keyW/2 + 3, y - 16, cx + keyW/2 - 3, y + 16); }
        h.setColor(srsel ? 3 : 2);
        h.drawString(keyLabel(row[c]), cx, y);
      }
    }
    footer("knob1: row   knob2: col   click: key");
  }

  function draw(){
    h.clear();
    if(view === "list") drawList();
    else if(view === "chat") drawChat();
    else drawCompose();
    h.flip();
  }

  // ---- compose actions ----
  function openCompose(c){ active = c; kb.buf = ""; kb.r = 1; kb.c = 0; kb.blink = true; view = "compose"; draw(); }
  function exitCompose(){ view = "chat"; draw(); }
  function selectKey(){
    var tok = KROWS[kb.r][kb.c];
    if(tok === "SP") kb.buf += " ";
    else if(tok === "DEL") kb.buf = kb.buf.slice(0, -1);
    else if(tok === "CANC") { exitCompose(); return; }
    else if(tok === "SEND") { if(kb.buf.length) sendMsg(active, kb.buf); exitCompose(); return; }
    else kb.buf += tok;
    draw();
  }

  // ---- input ----
  function openSel(){ var vis = visible(); if(!vis.length) return; active = vis[sel]; active.unread = 0; view = "chat"; chatScroll = -1; draw(); }
  function onK1(d){
    if(view === "list"){
      if(d === 0){ openSel(); return; }
      sel = Math.max(0, Math.min(visible().length - 1, sel + (d>0?1:-1))); draw();
    } else if(view === "chat"){
      if(d === 0){ openCompose(active); return; }
      var lines = buildChatLines(active), vis = chatVis(), maxStart = Math.max(0, lines.length - vis);
      var cur = (chatScroll < 0) ? maxStart : chatScroll;
      chatScroll = Math.max(0, Math.min(maxStart, cur + (d>0?1:-1))); draw();
    } else { // compose
      if(d === 0){ selectKey(); return; }
      kb.r = (kb.r + (d>0?1:-1) + KROWS.length) % KROWS.length;
      if(kb.c >= KROWS[kb.r].length) kb.c = KROWS[kb.r].length - 1;
      draw();
    }
  }
  function onK2(d){
    if(d === 0) return;
    if(view === "list"){
      filter = filter === "ch" ? "dm" : filter === "dm" ? "all" : "ch";
      sel = 0; draw();
    } else if(view === "chat"){ view = "list"; active = null; draw(); }
    else if(view === "compose"){
      var n = KROWS[kb.r].length;
      kb.c = (kb.c + (d>0?1:-1) + n) % n; draw();
    }
  }
  function bindInput(){
    Pip.removeAllListeners("knob1"); Pip.removeAllListeners("knob2");
    Pip.on("knob1", onK1); Pip.on("knob2", onK2);
  }

  // ---- start ----
  var simTimer = null;
  function setSim(on){
    if(on && !simTimer) simTimer = setInterval(simulate, 5000);
    else if(!on && simTimer){ clearInterval(simTimer); simTimer = null; }
  }

  bindInput();
  draw();
  timers.push(setInterval(function(){ if(rxFlash>0){ rxFlash--; if(rxFlash===0) draw(); } }, 500));
  timers.push(setInterval(function(){ if(view === "compose"){ kb.blink = !kb.blink; draw(); } }, 500));
  setSim(false);   // simulator off by default; real data comes from the bridge

  return {
    id: "MESH",
    rx: function(convoName, from, text, snr){ handleIncoming(convoName, from, text, snr); },
    setSim: setSim,
    reset: function(){ setSim(false); convos = []; sel = 0; active = null; view = "list"; chatScroll = 0; draw(); },
    addContact: function(name, kind){
      for(var i=0;i<convos.length;i++) if(convos[i].name === name) return;
      convos.push({ name:name, kind:kind||"dm", snr:0, unread:0, msgs:[] });
    },
    refresh: function(){ draw(); },
    remove: function(){
      for(var i=0;i<timers.length;i++) clearInterval(timers[i]);
      timers = [];
      setSim(false);
      Pip.removeAllListeners("knob1");
      Pip.removeAllListeners("knob2");
    }
  };
})