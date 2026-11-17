/* Port of main/ui.c draw_robot + font5x7 labels — same units, colours, motion. */
(function (global) {
  'use strict';

  const TWO_PI = Math.PI * 2;
  const C = {
    bgTop: '#0b1020', bgBot: '#171d33', text: '#e8ecf4', muted: '#7a8496',
    face: '#101526', shadow: '#050710', gray: '#6b7280', white: '#ffffff',
    keyboard: '#3a445c',
    idle: '#34d048', working: '#ff7a18', waiting: '#ffd400', error: '#ff3d9a',
    claude: '#d97757', claudeShade: '#a85036',
    copilot: '#7c6cf0', copilotShade: '#5244c0',
    grok: '#c9d1dc', grokShade: '#8a93a0',
    other: '#2bb3a3', otherShade: '#1c8074',
  };

  function stateColor(st) {
    return ({ idle: C.idle, working: C.working, waiting: C.waiting, error: C.error }[st]) || C.muted;
  }
  function stateLabel(st) {
    return ({ idle: 'IDLE', working: 'WORKING', waiting: 'WAITING', error: 'ERROR', offline: 'OFFLINE' }[st]) || 'OFFLINE';
  }
  function kindColors(kind) {
    if (kind === 'claude') return [C.claude, C.claudeShade];
    if (kind === 'copilot') return [C.copilot, C.copilotShade];
    if (kind === 'grok') return [C.grok, C.grokShade];
    return [C.other, C.otherShade];
  }

  function mix(a, b, t /* 0..255 */) {
    if (t <= 0) return a;
    if (t >= 255) return b;
    const pa = parseInt(a.slice(1), 16), pb = parseInt(b.slice(1), 16);
    const ar = pa >> 16, ag = (pa >> 8) & 255, ab = pa & 255;
    const br = pb >> 16, bg = (pb >> 8) & 255, bb = pb & 255;
    const r = ar + ((br - ar) * t) / 255 | 0;
    const g = ag + ((bg - ag) * t) / 255 | 0;
    const bl = ab + ((bb - ab) * t) / 255 | 0;
    return '#' + ((1 << 24) | (r << 16) | (g << 8) | bl).toString(16).slice(1);
  }

  /* 5x7 glyphs from main/font5x7.c (A–Z, 0–9, punctuation used on the wall). */
  const FONT = {
    ' ': [0x00, 0x00, 0x00, 0x00, 0x00],
    '!': [0x00, 0x00, 0x5F, 0x00, 0x00],
    '?': [0x02, 0x01, 0x51, 0x09, 0x06],
    '-': [0x08, 0x08, 0x08, 0x08, 0x08],
    '.': [0x00, 0x60, 0x60, 0x00, 0x00],
    '0': [0x3E, 0x51, 0x49, 0x45, 0x3E],
    '1': [0x00, 0x42, 0x7F, 0x40, 0x00],
    '2': [0x42, 0x61, 0x51, 0x49, 0x46],
    '3': [0x21, 0x41, 0x45, 0x4B, 0x31],
    '4': [0x18, 0x14, 0x12, 0x7F, 0x10],
    '5': [0x27, 0x45, 0x45, 0x45, 0x39],
    '6': [0x3C, 0x4A, 0x49, 0x49, 0x30],
    '7': [0x01, 0x71, 0x09, 0x05, 0x03],
    '8': [0x36, 0x49, 0x49, 0x49, 0x36],
    '9': [0x06, 0x49, 0x49, 0x29, 0x1E],
    A: [0x7E, 0x11, 0x11, 0x11, 0x7E],
    B: [0x7F, 0x49, 0x49, 0x49, 0x36],
    C: [0x3E, 0x41, 0x41, 0x41, 0x22],
    D: [0x7F, 0x41, 0x41, 0x22, 0x1C],
    E: [0x7F, 0x49, 0x49, 0x49, 0x41],
    F: [0x7F, 0x09, 0x09, 0x09, 0x01],
    G: [0x3E, 0x41, 0x49, 0x49, 0x7A],
    H: [0x7F, 0x08, 0x08, 0x08, 0x7F],
    I: [0x00, 0x41, 0x7F, 0x41, 0x00],
    J: [0x20, 0x40, 0x41, 0x3F, 0x01],
    K: [0x7F, 0x08, 0x14, 0x22, 0x41],
    L: [0x7F, 0x40, 0x40, 0x40, 0x40],
    M: [0x7F, 0x02, 0x0C, 0x02, 0x7F],
    N: [0x7F, 0x04, 0x08, 0x10, 0x7F],
    O: [0x3E, 0x41, 0x41, 0x41, 0x3E],
    P: [0x7F, 0x09, 0x09, 0x09, 0x06],
    Q: [0x3E, 0x41, 0x51, 0x21, 0x5E],
    R: [0x7F, 0x09, 0x19, 0x29, 0x46],
    S: [0x46, 0x49, 0x49, 0x49, 0x31],
    T: [0x01, 0x01, 0x7F, 0x01, 0x01],
    U: [0x3F, 0x40, 0x40, 0x40, 0x3F],
    V: [0x1F, 0x20, 0x40, 0x20, 0x1F],
    W: [0x3F, 0x40, 0x38, 0x40, 0x3F],
    X: [0x63, 0x14, 0x08, 0x14, 0x63],
    Y: [0x07, 0x08, 0x70, 0x08, 0x07],
    Z: [0x61, 0x51, 0x49, 0x45, 0x43],
    z: [0x44, 0x64, 0x54, 0x4C, 0x44],
  };

  function textWidth(s, scale) {
    return s.length * 6 * scale;
  }
  function drawText(ctx, x, y, s, scale, color) {
    ctx.fillStyle = color;
    for (let i = 0; i < s.length; i++) {
      const g = FONT[s[i]] || FONT[' '] || [0, 0, 0, 0, 0];
      for (let col = 0; col < 5; col++) {
        const bits = g[col];
        for (let row = 0; row < 7; row++) {
          if (bits & (1 << row)) {
            ctx.fillRect(x + (i * 6 + col) * scale, y + row * scale, scale, scale);
          }
        }
      }
    }
  }
  function drawTextCentered(ctx, cx, y, s, scale, color) {
    drawText(ctx, (cx - textWidth(s, scale) / 2) | 0, y, s, scale, color);
  }

  function fillRoundRect(ctx, x, y, w, h, r) {
    if (w < 1 || h < 1) return;
    r = Math.max(0, Math.min(r, w / 2, h / 2));
    ctx.beginPath();
    if (typeof ctx.roundRect === 'function') {
      ctx.roundRect(x, y, w, h, r);
    } else {
      ctx.moveTo(x + r, y);
      ctx.arcTo(x + w, y, x + w, y + h, r);
      ctx.arcTo(x + w, y + h, x, y + h, r);
      ctx.arcTo(x, y + h, x, y, r);
      ctx.arcTo(x, y, x + w, y, r);
      ctx.closePath();
    }
    ctx.fill();
  }

  function fig(ctx, k, cx, top) {
    const FX = (u) => cx + Math.round(u * k);
    const FY = (u) => top + Math.round(u * k);
    const FS = (u) => Math.max(1, Math.round(u * k));
    return {
      rrect(x, y, w, h, r, c) {
        const x0 = FX(x), y0 = FY(y), ww = FX(x + w) - x0, hh = FY(y + h) - y0, rr = FS(r);
        ctx.fillStyle = c;
        fillRoundRect(ctx, x0, y0, ww, hh, rr);
      },
      circle(x, y, r, c) {
        /* Integer disk — matches the firmware's chunky filled circles. */
        const cx0 = FX(x), cy0 = FY(y), rr = FS(r);
        ctx.fillStyle = c;
        for (let dy = -rr; dy <= rr; dy++) {
          for (let dx = -rr; dx <= rr; dx++) {
            if (dx * dx + dy * dy <= rr * rr) ctx.fillRect(cx0 + dx, cy0 + dy, 1, 1);
          }
        }
      },
      line(x0, y0, x1, y1, t, c) {
        const thick = FS(t);
        const ax = FX(x0), ay = FY(y0), bx = FX(x1), by = FY(y1);
        const steps = Math.max(1, Math.abs(bx - ax) + Math.abs(by - ay));
        ctx.fillStyle = c;
        for (let i = 0; i <= steps; i++) {
          const px = Math.round(ax + ((bx - ax) * i) / steps);
          const py = Math.round(ay + ((by - ay) * i) / steps);
          const hr = Math.max(1, (thick / 2) | 0);
          for (let dy = -hr; dy <= hr; dy++) {
            for (let dx = -hr; dx <= hr; dx++) {
              if (dx * dx + dy * dy <= hr * hr) ctx.fillRect(px + dx, py + dy, 1, 1);
            }
          }
        }
      },
      ellipse(x, y, rx, ry, c) {
        const cx0 = FX(x), cy0 = FY(y), rx0 = FS(rx), ry0 = FS(ry);
        ctx.fillStyle = c;
        for (let dy = -ry0; dy <= ry0; dy++) {
          for (let dx = -rx0; dx <= rx0; dx++) {
            if ((ry0 ? (dx * dx) / (rx0 * rx0) + (dy * dy) / (ry0 * ry0) : dx * dx) <= 1)
              ctx.fillRect(cx0 + dx, cy0 + dy, 1, 1);
          }
        }
      },
      ring(x, y, rOut, rIn, c) {
        const cx0 = FX(x), cy0 = FY(y), ro = FS(rOut), ri = FS(rIn);
        ctx.fillStyle = c;
        for (let dy = -ro; dy <= ro; dy++) {
          for (let dx = -ro; dx <= ro; dx++) {
            const d2 = dx * dx + dy * dy;
            if (d2 <= ro * ro && d2 >= ri * ri) ctx.fillRect(cx0 + dx, cy0 + dy, 1, 1);
          }
        }
      },
      triangle(x0, y0, x1, y1, x2, y2, c) {
        ctx.fillStyle = c;
        ctx.beginPath();
        ctx.moveTo(FX(x0), FY(y0));
        ctx.lineTo(FX(x1), FY(y1));
        ctx.lineTo(FX(x2), FY(y2));
        ctx.closePath();
        ctx.fill();
      },
      textAt(x, y, s, scUnits, c) {
        const sc = Math.max(1, Math.round(k * scUnits));
        drawTextCentered(ctx, FX(x) + sc / 2, FY(y) - (7 * sc) / 2, s, sc, c);
      },
      FX, FY, FS, k,
    };
  }

  function drawBellyIcon(f, kind, cx, cy, c) {
    if (kind === 'claude') {
      const lens = [7.5, 6, 7, 5.5, 7.5, 6.5, 7, 5.5, 7.5, 6, 6.5, 5.5];
      for (let i = 0; i < 12; i++) {
        const a = i * (TWO_PI / 12) + 0.13;
        const ca = Math.cos(a), sa = Math.sin(a);
        f.line(cx + 1.5 * ca, cy + 1.5 * sa, cx + lens[i] * ca, cy + lens[i] * sa, 1.8, c);
      }
      f.circle(cx, cy, 2.2, c);
    } else if (kind === 'copilot') {
      f.circle(cx - 10, cy + 1, 2.2, c);
      f.circle(cx + 10, cy + 1, 2.2, c);
      f.rrect(cx - 10, cy - 7, 20, 15, 6, c);
      for (const side of [-1, 1]) {
        const gx = cx + side * 4.5;
        f.rrect(gx - 3.5, cy - 5, 7, 5, 2, C.face);
        f.rrect(cx + side * 2.5 - 0.8, cy + 2, 1.6, 3.5, 0.8, C.face);
      }
    } else if (kind === 'grok') {
      const r = 5.5, a0 = -0.15, a1 = 4.25, segs = 14;
      for (let i = 0; i < segs; i++) {
        const p = a0 + (a1 - a0) * i / segs, q = a0 + (a1 - a0) * (i + 1) / segs;
        f.line(cx + r * Math.cos(p), cy + r * Math.sin(p), cx + r * Math.cos(q), cy + r * Math.sin(q), 1.8, c);
      }
      f.line(cx - 6.5, cy + 6.5, cx + 7.5, cy - 7.5, 2.0, c);
    }
  }

  function drawBubble(f, cx, cy, w, h, tailX, tailY, c, text, textC) {
    f.triangle(cx - w * 0.25, cy + h * 0.3, cx - w * 0.05, cy + h * 0.45, tailX, tailY, c);
    f.rrect(cx - w / 2, cy - h / 2, w, h, h * 0.4, c);
    if (text) f.textAt(cx, cy, text, h / 12, textC);
  }

  function drawRobot(ctx, kind, st, t, k, cx, top) {
    const f = fig(ctx, k, cx, top);
    const ph = t / 1000;
    let acc = stateColor(st);
    let body, shade;
    if (st === 'offline') {
      [body, shade] = kindColors(kind);
      body = mix(body, C.gray, 190);
      shade = mix(shade, C.gray, 190);
    } else {
      body = acc;
      if (st === 'waiting') {
        const on = (t % 800) < 400;
        if (!on) body = mix(acc, C.shadow, 170);
      }
      shade = mix(body, C.shadow, 90);
    }
    const dark = mix(shade, C.shadow, 110);

    let lift = 0, bob = 0, ox = 0;
    if (st === 'idle') bob = 2.0 * Math.sin(ph * TWO_PI / 2.4);
    else if (st === 'working') bob = -1.6 * Math.abs(Math.sin(ph * TWO_PI / 0.5));
    else if (st === 'waiting') lift = 0; /* stand still; only the waving arm moves */
    else if (st === 'error') ox = (t % 3000) < 700 ? 2.5 * Math.sin(ph * TWO_PI * 11) : 0;
    else bob = 0.8 * Math.sin(ph * TWO_PI / 4.0);

    const ub = lift + bob;
    const headY = 34 + lift + bob * 1.4;

    f.ellipse(0, 136, 30 + lift * 1.2, 4, C.shadow);

    for (const side of [-1, 1]) {
      f.rrect(ox + side * 12 - 5, 112 + ub, 10, 18 - bob, 3, shade);
      f.rrect(ox + side * 12 - 9, 126 + lift, 18, 7, 3, dark);
    }

    f.rrect(ox - 7, headY + 40, 14, 12, 2, shade);
    f.rrect(ox - 26, 84 + ub, 52, 34, 10, body);

    let pulse;
    if (st === 'working') pulse = (t % 300) < 150 ? 0 : 160;
    else if (st === 'waiting') pulse = (t % 800) < 400 ? 0 : 200;
    else if (st === 'offline') pulse = 200;
    else pulse = (90 + 90 * Math.sin(ph * TWO_PI / 2.0)) | 0;

    if (kind === 'claude' || kind === 'copilot' || kind === 'grok') {
      f.rrect(ox - 16, 91 + ub, 32, 20, 5, C.face);
      let [brand] = kindColors(kind);
      if (st === 'offline') brand = C.gray;
      drawBellyIcon(f, kind, ox, 101 + ub, mix(brand, C.face, pulse / 3));
    } else {
      f.rrect(ox - 16, 91 + ub, 32, 20, 5, shade);
      f.circle(ox, 101 + ub, 4.5, mix(acc, shade, pulse));
    }

    let tapL = 0, tapR = 0;
    if (st === 'working') {
      f.rrect(ox - 32, 114 + ub, 64, 8, 2, C.keyboard);
      for (let i = 0; i < 7; i++) {
        const lit = (((t / 120) | 0) + i * 3) % 7 === 0;
        f.rrect(ox - 28 + i * 8, 116 + ub, 6, 3, 1, lit ? acc : mix(C.keyboard, C.white, 60));
      }
      tapL = Math.sin(ph * TWO_PI * 3.0) > 0 ? -3 : 0;
      tapR = Math.sin(ph * TWO_PI * 3.0 + 1.8) > 0 ? -3 : 0;
    }

    const sy = 92 + ub;
    let aL = 100, aR = 80;
    if (st === 'idle') {
      const sw = 6 * Math.sin(ph * TWO_PI / 2.4);
      aL = 102 + sw; aR = 78 - sw;
    } else if (st === 'waiting') {
      aL = 104;
      aR = -45 + 28 * Math.sin(ph * TWO_PI / 0.7);
    } else if (st === 'error') {
      const sw = 8 * Math.sin(ph * TWO_PI * 2);
      aL = -130 + sw; aR = -50 - sw;
    } else if (st === 'offline') {
      aL = 95; aR = 85;
    }
    const armLen = 22, armT = 7, handR = 5;
    for (const side of [-1, 1]) {
      const shx = ox + side * 25;
      let hx, hy;
      if (st === 'working') {
        hx = ox + side * 13;
        hy = 113 + ub + (side < 0 ? tapL : tapR);
      } else {
        const a = ((side < 0 ? aL : aR) * TWO_PI) / 360;
        hx = shx + armLen * Math.cos(a);
        hy = sy + armLen * Math.sin(a);
      }
      f.line(shx, sy, hx, hy, armT, shade);
      f.circle(hx, hy, handR, body);
    }

    let ant = acc;
    if (st === 'working') ant = (t % 400) < 200 ? acc : mix(acc, C.white, 120);
    else if (st === 'waiting') ant = (t % 800) < 400 ? acc : mix(acc, shade, 200);
    else if (st === 'offline') ant = C.gray;

    if (kind === 'copilot') {
      for (const side of [-1, 1]) {
        f.line(ox + side * 14, headY + 2, ox + side * 21, headY - 9, 3, shade);
        f.circle(ox + side * 21, headY - 10, 4, ant);
      }
    } else if (kind === 'grok') {
      f.line(ox, headY + 2, ox + 8, headY - 9, 3, shade);
      f.circle(ox + 9, headY - 11, 4.5, ant);
    } else {
      f.line(ox, headY + 2, ox, headY - 9, 3, shade);
      f.circle(ox, headY - 12, 5, ant);
    }

    /* Head: thick body-coloured frame, dark screen inside — same as the LCD close-up. */
    f.circle(ox - 33, headY + 22, 4, shade);
    f.circle(ox + 33, headY + 22, 4, shade);
    f.rrect(ox - 32, headY, 64, 44, 13, body);
    f.rrect(ox - 26, headY + 7, 52, 31, 8, C.face);

    let eye = st === 'offline' ? C.gray : acc;
    let look = 0, ew = 8, eh = 13, ex = 12, ey = headY + 20;
    if (st === 'idle') {
      look = 2.0 * Math.sin(ph * TWO_PI / 5.0);
      if (t % 3600 < 140) eh = 2;
    } else if (st === 'working') {
      look = 3.0 * Math.sin(ph * TWO_PI / 1.2);
      ew = 9; eh = 7; ey += 1;
    } else if (st === 'waiting') {
      ew = 9; eh = 15;
      if (t % 2500 < 120) eh = 2;
    } else if (st === 'offline') {
      ew = 9; eh = 2; ey += 2;
    }

    for (const side of [-1, 1]) {
      const x = ox + side * ex + look;
      if (st === 'error') {
        f.line(x - 4, ey - 4, x + 4, ey + 4, 2.5, eye);
        f.line(x - 4, ey + 4, x + 4, ey - 4, 2.5, eye);
        continue;
      }
      /* Egg eyes: tall ellipses, not rounded rects / goggle discs. */
      f.ellipse(x, ey, ew / 2, eh / 2, eye);
      if (eh > 6) f.circle(x - ew / 5, ey - eh / 5, 1.4, mix(eye, C.white, 200));
    }

    let mc = mix(acc, C.face, 60);
    if (st === 'offline') mc = C.gray;
    const my = headY + 31;
    if (st === 'idle') {
      f.line(ox - 5, my - 1, ox - 2, my + 1, 2, mc);
      f.line(ox - 2, my + 1, ox + 2, my + 1, 2, mc);
      f.line(ox + 2, my + 1, ox + 5, my - 1, 2, mc);
    } else if (st === 'working') {
      const w = 3 + 1.5 * Math.sin(ph * TWO_PI * 2);
      f.line(ox - w, my, ox + w, my, 2, mc);
    } else if (st === 'waiting') {
      f.ring(ox, my, 3.2, 1.6, mc);
    } else if (st === 'error') {
      f.line(ox - 6, my, ox - 3, my - 2, 1.6, mc);
      f.line(ox - 3, my - 2, ox, my, 1.6, mc);
      f.line(ox, my, ox + 3, my - 2, 1.6, mc);
      f.line(ox + 3, my - 2, ox + 6, my, 1.6, mc);
    } else {
      f.line(ox - 3, my, ox + 3, my, 1.6, mc);
    }

    if (st === 'working') {
      /* Same bubble shape as waiting, orange, with typing dots instead of ?/!. */
      const b = 2 * Math.sin(ph * TWO_PI / 0.8);
      const bx = 40, by = 10 + b + bob;
      drawBubble(f, bx, by, 30, 22, 26, 30 + bob, acc, null, C.face);
      for (let i = 0; i < 3; i++) {
        let up = Math.sin(ph * TWO_PI * 1.5 - i * 0.9);
        if (up < 0) up = 0;
        f.circle(bx - 9 + i * 9, by - 2.5 * up, 2.6, mix(C.face, acc, (30 + up * 90) | 0));
      }
    } else if (st === 'waiting') {
      drawBubble(f, 40, 10, 30, 22, 26, 30, acc, '?', C.face);
    } else if (st === 'error') {
      drawBubble(f, 40, 10, 26, 20, 26, 30, acc, '!', C.face);
    } else if (st === 'offline') {
      for (let i = 0; i < 3; i++) {
        const p = ((ph / 3 + i / 3) % 1);
        const sc = Math.max(1, f.k * (1 + p * 1.2));
        const zc = mix(C.muted, C.bgTop, (p * 230) | 0);
        drawText(ctx, f.FX(24 + p * 22), f.FY(30 - p * 30), 'z', sc | 0, zc);
      }
    }
  }

  /* Draw at Waveshare-ish resolution, then nearest-neighbour upscale — same chunky look as the LCD. */
  const LCD_W = 172;
  const LCD_H = 320;
  let lcdCanvas = null;

  function safeText(s, maxLen) {
    const raw = String(s || '').toUpperCase();
    let out = '';
    for (const ch of raw) {
      if (FONT[ch]) out += ch;
      else if (ch === ' ' || ch === '?' || ch === '!' || ch === '.' || ch === '-' || ch === '/') out += ch;
    }
    if (maxLen && out.length > maxLen) out = out.slice(0, Math.max(0, maxLen - 2)) + '..';
    return out;
  }

  function formatAge(sec) {
    sec = Math.max(0, sec | 0);
    if (sec < 60) return 'FOR ' + sec + 'S';
    if (sec < 3600) return 'FOR ' + ((sec / 60) | 0) + 'M';
    if (sec < 86400) return 'FOR ' + ((sec / 3600) | 0) + 'H';
    return 'FOR ' + ((sec / 86400) | 0) + 'D';
  }

  function paintCard(canvas, agent, t) {
    if (!lcdCanvas) lcdCanvas = document.createElement('canvas');
    lcdCanvas.width = LCD_W;
    lcdCanvas.height = LCD_H;
    const lctx = lcdCanvas.getContext('2d');
    lctx.imageSmoothingEnabled = false;

    const g = lctx.createLinearGradient(0, 0, 0, LCD_H);
    g.addColorStop(0, C.bgTop);
    g.addColorStop(1, C.bgBot);
    lctx.fillStyle = g;
    lctx.fillRect(0, 0, LCD_W, LCD_H);

    const st = agent.state || 'offline';
    const kind = agent.kind || 'other';
    let acc = stateColor(st);
    if (st === 'offline') acc = C.muted;

    /* Same portrait stack as draw_normal on the LCD: kind + name, figure, state, detail, age. */
    const nameScale = 3;
    const hdrH = 14 + 7 * nameScale + 8;
    const big = 3;
    const labelH = 7 * big;
    const lineH = 7;
    const gap = 6;
    const infoH = labelH + gap + lineH + 12 + lineH + 10;
    const figAreaH = LCD_H - hdrH - infoH;
    const k = Math.min(LCD_W / 120, (figAreaH - 6) / 142);
    const cx = LCD_W / 2;
    const top = hdrH + (figAreaH - 142 * k) / 2 + 2;

    const kindLabel = safeText(kind, 10);
    if (kindLabel) drawText(lctx, 4, 4, kindLabel, 1, mix(acc, C.muted, 40));
    const name = safeText(agent.name || agent.id || 'AGENT', 12);
    drawTextCentered(lctx, LCD_W / 2, 14, name, nameScale, C.text);
    lctx.fillStyle = mix(C.bgTop, acc, 140);
    lctx.fillRect(8, hdrH - 4, LCD_W - 16, 1);

    drawRobot(lctx, kind, st, t, k, cx, top);

    let lc = acc;
    if (st === 'waiting' && (t % 1000) >= 700) lc = mix(lc, C.bgBot, 150);
    const infoY = hdrH + figAreaH;
    drawTextCentered(lctx, LCD_W / 2, infoY, stateLabel(st), big, lc);

    let y = infoY + labelH + gap;
    const detail = safeText(agent.detail, 22);
    if (detail) {
      drawTextCentered(lctx, LCD_W / 2, y, detail, 1, C.text);
    }
    y += 12;
    const since = Number(agent.since);
    const ageS = Number.isFinite(since) ? Math.max(0, ((Date.now() / 1000) - since) | 0) : 0;
    drawTextCentered(lctx, LCD_W / 2, y, formatAge(ageS), 1, C.muted);

    /* Upscale to the card canvas. */
    const cssW = canvas.clientWidth || LCD_W;
    const cssH = canvas.clientHeight || LCD_H;
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const W = Math.round(cssW * dpr), H = Math.round(cssH * dpr);
    if (canvas.width !== W || canvas.height !== H) {
      canvas.width = W;
      canvas.height = H;
    }
    const ctx = canvas.getContext('2d');
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.imageSmoothingEnabled = false;
    ctx.clearRect(0, 0, W, H);
    ctx.drawImage(lcdCanvas, 0, 0, W, H);
  }

  global.AgentRobot = { paintCard, stateLabel, stateColor, C };
})(window);
