(async function() {
    let decoder = new TextDecoder();
    const memory = new WebAssembly.Memory({ initial: 17 });

    function readCString(ptr) {
        const bytes = new Uint8Array(memory.buffer, ptr);
        let end = 0;
        while (bytes[end] !== 0) end++;
        return decoder.decode(bytes.subarray(0, end));
    }

    function readString(ptr, len) {
        return decoder.decode(new Uint8Array(memory.buffer, ptr, len));
    }

    function jsLog(ptr, len) {
        console.log(decoder.decode(new Uint8Array(memory.buffer, ptr, len)));
    }

    /* Dispatch a WMF opcode stream (emitted by wmf_parse in C) onto a 2D
       canvas, then export as a PNG blob. Mirrors refs/wmf/render.js. */
    const WMF_OP = Object.freeze({
        END: 0x00, BOUNDS: 0x01,
        SET_PEN: 0x02, SET_BRUSH: 0x03, SET_FONT: 0x04,
        SET_TEXT_COLOR: 0x05, SET_POLY_FILL_MODE: 0x06,
        POLYLINE: 0x07, POLYGON: 0x08, POLYPOLYGON: 0x09,
        TEXT: 0x0A,
        CLIP_SAVE: 0x0B, CLIP_RESTORE: 0x0C, CLIP_INTERSECT: 0x0D,
        DIB_BLIT: 0x0E, BIT_COPY: 0x0F,
        SET_WINDOW: 0x10,
    });

    /* COLORREF (0x00BBGGRR) -> CSS #RRGGBB. */
    function wmfColor(c) {
        return '#' +
            (c & 0xff).toString(16).padStart(2, '0') +
            ((c >> 8) & 0xff).toString(16).padStart(2, '0') +
            ((c >> 16) & 0xff).toString(16).padStart(2, '0');
    }

    /* Map a Windows GDI CharSet byte to a TextDecoder encoding label.
       Browsers natively support the SBCS windows-12xx pages and the DBCS
       legacy encodings (shift-jis, gbk, euc-kr, big5), so we don't need
       any conversion tables in C. */
    function wmfCharsetLabel(cs) {
        switch (cs) {
            case 128: return 'shift-jis';      /* SHIFTJIS */
            case 129: return 'euc-kr';         /* HANGUL — covers CP949 */
            case 134: return 'gbk';            /* GB2312 */
            case 136: return 'big5';           /* CHINESEBIG5 */
            case 161: return 'windows-1253';   /* GREEK */
            case 162: return 'windows-1254';   /* TURKISH */
            case 163: return 'windows-1258';   /* VIETNAMESE */
            case 177: return 'windows-1255';   /* HEBREW */
            case 178: return 'windows-1256';   /* ARABIC */
            case 186: return 'windows-1257';   /* BALTIC */
            case 204: return 'windows-1251';   /* RUSSIAN */
            case 222: return 'windows-874';    /* THAI */
            case 238: return 'windows-1250';   /* EASTEUROPE */
            default:  return 'windows-1252';   /* ANSI / DEFAULT / SYMBOL / OEM */
        }
    }
    const wmfDecoders = new Map();
    function wmfDecoder(cs) {
        let d = wmfDecoders.get(cs);
        if (d) return d;
        try { d = new TextDecoder(wmfCharsetLabel(cs), { fatal: false }); }
        catch (e) { d = new TextDecoder('windows-1252', { fatal: false }); }
        wmfDecoders.set(cs, d);
        return d;
    }

    async function renderWmfToBlob(opsBytes) {
        const dv = new DataView(opsBytes.buffer, opsBytes.byteOffset, opsBytes.byteLength);
        let p = 0;
        const rd_u8  = () => opsBytes[p++];
        const rd_u16 = () => { const v = dv.getUint16(p, true); p += 2; return v; };
        const rd_i16 = () => { const v = dv.getInt16(p, true);  p += 2; return v; };
        const rd_u32 = () => { const v = dv.getUint32(p, true); p += 4; return v; };

        if (opsBytes.length < 1 || rd_u8() !== WMF_OP.BOUNDS)
            throw new Error('wmf: missing BOUNDS opcode');
        const orgX = rd_i16(), orgY = rd_i16();
        const extX = rd_i16(), extY = rd_i16();
        const w = Math.abs(extX), h = Math.abs(extY);
        if (!w || !h) throw new Error('wmf: zero extents');

        const MAX_DIM = 512;
        const scale = Math.min(1, MAX_DIM / Math.max(w, h));
        const cw = Math.max(1, Math.round(w * scale));
        const ch = Math.max(1, Math.round(h * scale));

        const canvas = document.createElement('canvas');
        canvas.width = cw;
        canvas.height = ch;
        const ctx = canvas.getContext('2d');
        /* GDI doesn't anti-alias drawing primitives. We disable both AA
           knobs to match: imageSmoothingEnabled controls drawImage
           interpolation (drives DIB blits); ctx.antialias is a
           node-canvas extension for path AA — browsers ignore it but
           it's harmless to set, and keeps main.js consistent with the
           validator and refs/wmf/render.js. */
        ctx.imageSmoothingEnabled = false;
        ctx.antialias = 'none';
        /* Initial transform mirrors refs/wmf/render.js exactly so files
           that don't change the window mid-stream stay pixel-identical
           to the baselines (scale*sign, then translate; round() inside
           cw/ch alone would drift one pixel on many files). */
        ctx.scale(scale * (extX < 0 ? -1 : 1), scale * (extY < 0 ? -1 : 1));
        ctx.translate(-orgX, -orgY);

        /* SET_WINDOW (mid-stream SetWindowOrg/Ext changes) maps a new
           logical window into the same device pixels (cw, ch) — by
           definition not the original window, so we use cw/ch directly. */
        const applyWindow = (oX, oY, eX, eY) => {
            if (!eX || !eY) return;
            const sx = cw / Math.abs(eX) * (eX < 0 ? -1 : 1);
            const sy = ch / Math.abs(eY) * (eY < 0 ? -1 : 1);
            ctx.setTransform(sx, 0, 0, sy, 0, 0);
            ctx.translate(-oX, -oY);
        };

        /* Snap a pen width up to the next integer device pixel so wider
           pens stay visibly thicker than 1-unit cosmetic pens after
           downscaling. lineWidth is in transformed coords so we divide
           back by scale. (Matches refs/wmf/render.js.) */
        const penWidth = (w) => {
            if (!scale) return Math.max(w || 0, 1);
            return Math.max(1, Math.ceil((w || 0) * scale)) / scale;
        };

        /* Track GDI pen/brush/fill-mode/font/text-color. Initial colors
           match Canvas2D's default fillStyle/strokeStyle ("#000000")
           so that WMFs which draw before any SelectObject reproduce
           render.js's behavior — render.js leaves fillStyle/strokeStyle
           untouched when state.Brush.Color / state.Pen.Color are
           undefined, leaving the canvas defaults in place. (Real GDI's
           default is the WHITE_BRUSH; render.js doesn't model that.) */
        let pen = { style: 0, width: 1, color: 0x000000 };
        let brush = { style: 0, color: 0x000000, hatch: 0 };
        let polyFillMode = 1; /* ALTERNATE / even-odd */
        let textColor = 0x000000;
        let fontAngleDeg = 0; /* tenths-of-degree → degrees applied at TEXT */
        let fontCharset = 0;

        const tracePoly = (n, closed) => {
            for (let i = 0; i < n; i++) {
                const x = rd_i16(), y = rd_i16();
                if (i === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            }
            if (closed) ctx.closePath();
        };

        while (p < opsBytes.length) {
            const op = rd_u8();
            if (op === WMF_OP.END) break;

            switch (op) {
            case WMF_OP.SET_PEN:
                pen.style = rd_u16();
                pen.width = rd_u16();
                pen.color = rd_u32();
                break;
            case WMF_OP.SET_BRUSH:
                brush.style = rd_u16();
                brush.color = rd_u32();
                brush.hatch = rd_u16();
                break;
            case WMF_OP.SET_POLY_FILL_MODE:
                polyFillMode = rd_u8();
                break;

            case WMF_OP.SET_FONT: {
                const height  = rd_i16();
                const weight  = rd_u16();
                const italic  = rd_u8();
                const angle   = rd_i16(); /* tenths of a degree */
                const charset = rd_u8();
                const nlen    = rd_u16();
                const name    = wmfDecoder(charset).decode(opsBytes.subarray(p, p + nlen));
                p += nlen;
                let s = '';
                if (italic) s += 'italic ';
                if (weight === 700) s += 'bold ';
                else if (weight && weight !== 400) s += weight + ' ';
                const px = Math.abs(height);
                if (px) s += px + 'px ';
                let face = name;
                if (face === 'System') face = 'Calibri';
                s += face ? `'${face}', sans-serif` : 'sans-serif';
                ctx.font = s.trim();
                fontAngleDeg = angle / 10;
                fontCharset = charset;
                break;
            }
            case WMF_OP.SET_TEXT_COLOR:
                textColor = rd_u32();
                break;
            case WMF_OP.TEXT: {
                const x = rd_i16(), y = rd_i16();
                const nlen = rd_u16();
                const text = wmfDecoder(fontCharset).decode(opsBytes.subarray(p, p + nlen));
                p += nlen;
                ctx.fillStyle = wmfColor(textColor);
                if (fontAngleDeg) {
                    ctx.save();
                    ctx.translate(x, y);
                    ctx.rotate(-fontAngleDeg * Math.PI / 180);
                    ctx.fillText(text, 0, 0);
                    ctx.restore();
                } else {
                    ctx.fillText(text, x, y);
                }
                break;
            }

            case WMF_OP.POLYLINE: {
                const n = rd_u16();
                if (!n) break;
                ctx.beginPath();
                tracePoly(n, false);
                ctx.strokeStyle = wmfColor(pen.color);
                ctx.lineWidth = penWidth(pen.width);
                if (pen.style !== 5) ctx.stroke();
                break;
            }
            case WMF_OP.POLYGON: {
                const n = rd_u16();
                if (!n) break;
                ctx.beginPath();
                tracePoly(n, true);
                ctx.strokeStyle = wmfColor(pen.color);
                ctx.fillStyle   = wmfColor(brush.color);
                ctx.lineWidth   = penWidth(pen.width);
                /* Fill before stroke so the stroke paints on top of the
                   fill (matches GDI; otherwise the fill covers the inner
                   half of the stroke and outlines look too thin). */
                if (brush.style !== 1) {
                    ctx.fill(polyFillMode === 2 ? 'nonzero' : 'evenodd');
                }
                if (pen.style !== 5) ctx.stroke();
                break;
            }
            case WMF_OP.POLYPOLYGON: {
                const np = rd_u16();
                if (!np) break;
                const sizes = new Array(np);
                for (let i = 0; i < np; i++) sizes[i] = rd_u16();
                ctx.beginPath();
                for (let i = 0; i < np; i++) tracePoly(sizes[i], true);
                ctx.strokeStyle = wmfColor(pen.color);
                ctx.fillStyle   = wmfColor(brush.color);
                ctx.lineWidth   = penWidth(pen.width);
                /* Sub-polygons fill as a single path under the current
                   fill rule. With ALTERNATE (the WMF default) nested
                   contours punch holes — clip-art draws thick outlines
                   as one PolyPolygon and relies on this. */
                if (brush.style !== 1) {
                    ctx.fill(polyFillMode === 2 ? 'nonzero' : 'evenodd');
                }
                if (pen.style !== 5) ctx.stroke();
                break;
            }

            case WMF_OP.CLIP_SAVE:    ctx.save();    break;
            case WMF_OP.CLIP_RESTORE: ctx.restore(); break;
            case WMF_OP.CLIP_INTERSECT: {
                const np = rd_u16();
                const sizes = new Array(np);
                for (let i = 0; i < np; i++) sizes[i] = rd_u16();
                ctx.beginPath();
                for (let i = 0; i < np; i++) tracePoly(sizes[i], true);
                ctx.clip(polyFillMode === 2 ? 'nonzero' : 'evenodd');
                break;
            }

            case WMF_OP.DIB_BLIT: {
                const dx = rd_i16(), dy = rd_i16();
                const dw = rd_i16(), dh = rd_i16();
                const w  = rd_u16(), h  = rd_u16();
                /* Top-down RGBA payload, w*h*4 bytes. Wrap as
                   ImageData, paint into an offscreen canvas, then
                   drawImage so the current transform applies — unlike
                   putImageData, which works in raw device pixels. */
                const rgba = new Uint8ClampedArray(
                    opsBytes.buffer, opsBytes.byteOffset + p, w * h * 4);
                p += w * h * 4;
                const off = document.createElement('canvas');
                off.width = w; off.height = h;
                off.getContext('2d').putImageData(new ImageData(rgba, w, h), 0, 0);
                if (dw >= 0 && dh >= 0) {
                    ctx.drawImage(off, dx, dy, dw || w, dh || h);
                } else {
                    ctx.save();
                    ctx.translate(dx, dy);
                    ctx.scale(dw < 0 ? -1 : 1, dh < 0 ? -1 : 1);
                    ctx.drawImage(off, 0, 0, Math.abs(dw) || w, Math.abs(dh) || h);
                    ctx.restore();
                }
                break;
            }
            case WMF_OP.SET_WINDOW: {
                const oX = rd_i16(), oY = rd_i16();
                const eX = rd_i16(), eY = rd_i16();
                applyWindow(oX, oY, eX, eY);
                break;
            }
            case WMF_OP.BIT_COPY: {
                const dx = rd_i16(), dy = rd_i16();
                const sx = rd_i16(), sy = rd_i16();
                const w  = rd_i16(), h  = rd_i16();
                if (w <= 0 || h <= 0) break;
                /* Canvas-to-canvas copy where both rects are in WMF
                   logical coords. drawImage's source rect is in image
                   (untransformed) space, so we map the logical source
                   rect through the current transform first; the dest
                   rect stays in user space and re-applies the
                   transform automatically. WMF transforms are
                   axis-aligned scale + translate (m.b = m.c = 0), so
                   the rect stays axis-aligned even when extents are
                   negative — we just normalize via min/abs. */
                const m = ctx.getTransform();
                const ix1 = m.a * sx + m.e;
                const ix2 = m.a * (sx + w) + m.e;
                const iy1 = m.d * sy + m.f;
                const iy2 = m.d * (sy + h) + m.f;
                const isx = Math.min(ix1, ix2);
                const isy = Math.min(iy1, iy2);
                const isw = Math.abs(ix2 - ix1);
                const ish = Math.abs(iy2 - iy1);
                if (isw > 0 && ish > 0) {
                    ctx.drawImage(canvas, isx, isy, isw, ish, dx, dy, w, h);
                }
                break;
            }

            default:
                console.warn('wmf: unhandled opcode 0x' + op.toString(16));
                /* Stop to avoid mis-reading subsequent bytes once we hit
                   an opcode whose payload size we don't know. */
                p = opsBytes.length;
                break;
            }
        }

        return new Promise((resolve, reject) => {
            canvas.toBlob(b => b ? resolve(b) : reject(new Error('toBlob failed')),
                          'image/png');
        });
    }

    const { instance } = await WebAssembly.instantiateStreaming(
        fetch('hlp.wasm?v=' + Date.now()),
        { env: { memory, js_log: jsLog } }
    );

    const wasm = instance.exports;
    window.wasm = wasm; // debug access
    const app = document.getElementById('app');
    const sticky = document.getElementById('sticky');
    const content = document.getElementById('content');

    function showError() {
        const errPtr = wasm.hlp_get_error();
        if (errPtr) content.textContent = 'Error: ' + readCString(errPtr);
    }

    /* Build CSS classes from font table */
    function buildFontStyles(handle) {
        const numFonts = wasm.hlp_get_num_fonts(handle);
        const fontsPtr = wasm.hlp_get_fonts(handle);
        const raw = new Uint8Array(memory.buffer);
        const mem = new DataView(memory.buffer);
        let css = '';

        for (let i = 0; i < numFonts; i++) {
            const base = fontsPtr + i * 47;
            const face = readCString(base);
            const halfPts = mem.getUint16(base + 32, true);
            const weight = mem.getUint16(base + 34, true);
            const italic = raw[base + 36];
            const underline = raw[base + 37];
            const strikeout = raw[base + 38];
            const smallCaps = raw[base + 39];
            const family = raw[base + 40];
            const r = raw[base + 41], g = raw[base + 42], b = raw[base + 43];

            const families = { 1: 'monospace', 2: 'serif', 3: 'sans-serif', 4: 'cursive', 5: 'fantasy' };
            const fallback = families[family] || 'sans-serif';
            const fontFamily = face ? `"${face}", ${fallback}` : fallback;

            let rule = `font-family: ${fontFamily}; font-size: ${halfPts / 2}pt; font-weight: ${weight};`;
            if (italic) rule += ' font-style: italic;';
            if (underline) rule += ' text-decoration: underline;';
            if (strikeout) rule += ' text-decoration: line-through;';
            if (smallCaps) rule += ' font-variant: small-caps;';
            if (r || g || b) rule += ` color: rgb(${r},${g},${b});`;

            css += `.hlp-font-${i} { ${rule} }\n`;
        }

        let style = document.getElementById('hlp-fonts');
        if (!style) {
            style = document.createElement('style');
            style.id = 'hlp-fonts';
            document.head.appendChild(style);
        }
        style.textContent = css;
    }

    /* Render opcode buffer to DOM */
    function renderOpcodes(ptr, len) {
        const buf = new DataView(memory.buffer, ptr, len);
        let off = 0;
        const rootContainer = document.createElement('div');
        rootContainer.className = 'hlp-page';
        let container = rootContainer;
        let currentPara = null;
        let currentSpan = null;
        let currentLink = null;
        let currentFontClass = '';
        const pendingMacros = [];
        let mapCounter = 0;
        let pendingParaFmt = null;
        let needNewBlock = true; /* first content creates a block */

        let inTable = false;
        let nsrParent = null;

        function applyParaBlock() {
            /* In table context, apply borders to the <td> but don't create a new div */
            if (inTable && currentPara && currentPara.tagName === 'TD') {
                needNewBlock = false;
                currentSpan = null;
                currentLink = null;
                const f = pendingParaFmt;
                if (f && f.borderFlags) {
                    const bf = f.borderFlags;
                    const thick = bf & 0x20;
                    const dbl = bf & 0x40;
                    const bdrStyle = dbl ? 'double' : 'solid';
                    const bdrW = thick ? (dbl ? 5 : 3) : (dbl ? 3 : 1);
                    const bdr = `${bdrW}px ${bdrStyle} #000`;
                    if (bf & 0x01) currentPara.style.border = bdr;
                    else {
                        if (bf & 0x02) currentPara.style.borderTop = bdr;
                        if (bf & 0x04) currentPara.style.borderLeft = bdr;
                        if (bf & 0x08) currentPara.style.borderBottom = bdr;
                        if (bf & 0x10) currentPara.style.borderRight = bdr;
                    }
                }
                return;
            }
            /* Create a new <div> block with the current paragraph formatting */
            currentPara = document.createElement('div');
            currentPara.className = 'hlp-para';
            currentSpan = null;
            currentLink = null;
            needNewBlock = false;

            const f = pendingParaFmt;
            if (!f) { container.appendChild(currentPara); return; }

            currentPara.id = 'topic-' + f.paraOffset;
            currentPara._tabStops = f.tabStops;

            /* Convert native units to points: value * scale / 20 */
            const tw = v => v * hlpScale / 20; /* native → twips → points */
            let style = '';
            if (f.spaceBefore) style += `margin-top: ${tw(f.spaceBefore)}pt; `;
            if (f.spaceAfter) style += `margin-bottom: ${tw(f.spaceAfter)}pt; `;
            if (f.lineSpace) style += `line-height: ${tw(Math.abs(f.lineSpace))}pt; min-height: ${tw(Math.abs(f.lineSpace))}pt; `;
            if (f.borderFlags) {
                const pLeft = f.indentLeft ? tw(f.indentLeft) : 4;
                const pRight = f.indentRight ? tw(f.indentRight) : 4;
                style += `padding-left: ${pLeft}pt; `;
                style += `padding-right: ${pRight}pt; `;
                if (f.indentFirst) style += `text-indent: ${tw(f.indentFirst)}pt; `;
                /* Border flags: bit0=box, bit1=top, bit2=left, bit3=bottom, bit4=right
                   bit5=thick, bit6=double */
                const bf = f.borderFlags;
                const thick = bf & 0x20;
                const dbl = bf & 0x40;
                const bdrStyle = dbl ? 'double' : 'solid';
                const bdrW = thick ? (dbl ? 5 : 3) : (dbl ? 3 : 1);
                const bdr = `${bdrW}px ${bdrStyle} #000`;
                if (bf & 0x01) { /* box — all sides */
                    style += `border: ${bdr}; `;
                } else {
                    if (bf & 0x02) style += `border-top: ${bdr}; `;
                    if (bf & 0x04) style += `border-left: ${bdr}; `;
                    if (bf & 0x08) style += `border-bottom: ${bdr}; `;
                    if (bf & 0x10) style += `border-right: ${bdr}; `;
                }
            } else {
                if (f.indentLeft) style += `margin-left: ${tw(f.indentLeft)}pt; `;
                if (f.indentRight) style += `margin-right: ${tw(f.indentRight)}pt; `;
                if (f.indentFirst) style += `text-indent: ${tw(f.indentFirst)}pt; `;
            }
            if (f.alignment === 1) style += 'text-align: right; ';
            if (f.alignment === 2) style += 'text-align: center; ';
            if (style) currentPara.style.cssText = style;

            /* Tab stops: absolute positions from paragraph left edge.
               CSS tab-size sets the interval, not absolute position.
               Use first tab stop as the tab width. */
            if (f.tabStops.length > 0) {
                currentPara.style.tabSize = tw(f.tabStops[0]) + 'pt';
            }

            currentPara.dataset.debug = `PARA off=${f.paraOffset} flags=0x${f.pflags.toString(16)} sb=${f.spaceBefore} sa=${f.spaceAfter} ls=${f.lineSpace} li=${f.indentLeft} fi=${f.indentFirst} tabs=[${f.tabStops}]`;

            container.appendChild(currentPara);
        }
        let title = '';
        let browseBwd = 0xFFFFFFFF;
        let browseFwd = 0xFFFFFFFF;

        function ensurePara() {
            if (!currentPara || needNewBlock) {
                applyParaBlock();
            }
        }

        function ensureSpan() {
            ensurePara();
            if (!currentSpan) {
                currentSpan = document.createElement('span');
                if (currentFontClass) currentSpan.className = currentFontClass;
                currentPara.appendChild(currentSpan);
            }
        }

        while (off < len) {
            const op = buf.getUint8(off++);
            switch (op) {
            case 0x01: { /* PAGE_START */
                const titleLen = buf.getUint32(off, true); off += 4;
                title = readString(ptr + off, titleLen); off += titleLen;
                browseBwd = buf.getUint32(off, true); off += 4;
                browseFwd = buf.getUint32(off, true); off += 4;
                break;
            }
            case 0x02: { /* PARAGRAPH — \pard: sets formatting and creates block */
                const paraOffset = buf.getUint32(off, true); off += 4;
                const pflags = buf.getUint16(off, true); off += 2;
                const spaceBefore = buf.getInt16(off, true); off += 2;
                const spaceAfter = buf.getInt16(off, true); off += 2;
                const lineSpace = buf.getInt16(off, true); off += 2;
                const indentLeft = buf.getInt16(off, true); off += 2;
                const indentRight = buf.getInt16(off, true); off += 2;
                const indentFirst = buf.getInt16(off, true); off += 2;
                const alignment = buf.getUint8(off++);
                const borderFlags = buf.getUint8(off++);
                const borderWidth = buf.getInt16(off, true); off += 2;
                const numTabs = buf.getUint8(off++);
                const tabStops = [];
                for (let t = 0; t < numTabs; t++) {
                    tabStops.push(buf.getUint16(off, true)); off += 2;
                }

                pendingParaFmt = {
                    paraOffset, pflags, spaceBefore, spaceAfter, lineSpace,
                    indentLeft, indentRight, indentFirst, alignment,
                    borderFlags, borderWidth, tabStops
                };

                /* Create a new block immediately */
                needNewBlock = true;
                break;
            }
            case 0x03: { /* TEXT */
                const tLen = buf.getUint16(off, true); off += 2;
                const text = readString(ptr + off, tLen); off += tLen;
                ensureSpan();
                currentSpan.appendChild(document.createTextNode(text));
                break;
            }
            case 0x04: { /* FONT_CHANGE */
                const fontIdx = buf.getUint16(off, true); off += 2;
                ensurePara();
                currentFontClass = `hlp-font-${fontIdx}`;
                currentSpan = document.createElement('span');
                currentSpan.className = currentFontClass;
                currentSpan.dataset.debug = `FONT idx=${fontIdx}`;
                (currentLink || currentPara).appendChild(currentSpan);
                break;
            }
            case 0x05: /* LINE_BREAK (0x81) — inline break within paragraph */
                ensurePara();
                currentPara.appendChild(document.createElement('br'));
                currentSpan = null;
                break;
            case 0x82: { /* \par — paragraph break, start a new block */
                const parToff = buf.getUint32(off, true); off += 4;
                /* Flush BEFORE updating paraOffset so empty paragraphs keep
                   their own offset (not the next paragraph's). */
                ensurePara();
                if (pendingParaFmt) pendingParaFmt.paraOffset = parToff;
                /* Tag the closing paragraph with the font active at \par — the
                   "paragraph mark font" — so ::before inherits it. */
                if (currentFontClass)
                    currentPara.classList.add(currentFontClass);
                needNewBlock = true;
                currentSpan = null;
                currentLink = null;
                break;
            }
            case 0x06: /* TAB */
                ensureSpan();
                currentSpan.appendChild(document.createTextNode('\t'));
                break;
            case 0x07: { /* LINK_START */
                const kind = buf.getUint8(off++);
                const hash = buf.getUint32(off, true); off += 4;
                const fileLen = buf.getUint16(off, true); off += 2;
                off += fileLen; /* file name (cross-file links, future use) */
                off += 2; /* window */

                ensurePara();
                const a = document.createElement('a');
                if (currentFontClass) a.classList.add(currentFontClass);
                if (kind <= 1) {
                    a.href = `#${currentFileKey}/topic-${hash}`;
                    a.classList.add(kind === 0 ? 'hlp-link-jump' : 'hlp-link-popup');
                    a.dataset.debug = `LINK kind=${kind} target=${hash}`;
                    a.addEventListener('click', e => {
                        e.preventDefault();
                        if (kind === 1) showPopup(hash, a);
                        else navigateTo(hash);
                    });
                } else {
                    a.href = '#';
                    a.classList.add('hlp-link-macro');
                    a.dataset.debug = `LINK kind=macro`;
                }
                currentPara.appendChild(a);
                currentLink = a;
                currentSpan = a;
                break;
            }
            case 0x08: /* LINK_END */
                currentLink = null;
                currentSpan = null;
                break;
            case 0x09: { /* IMAGE */
                const imgPos = buf.getUint8(off++);
                const bmIndex = buf.getUint32(off, true); off += 4;
                ensurePara();

                const img = document.createElement('img');
                img.style.verticalAlign = 'middle';
                (currentSpan || currentPara).appendChild(img);

                const peekType = wasm.hlp_peek_image_type(currentHandle, bmIndex);
                if (peekType === 8) {
                    const slotsPtr = wasm.malloc(8);
                    const rc = wasm.hlp_decode_image_wmf(currentHandle, bmIndex, slotsPtr, slotsPtr + 4);
                    if (rc === 0) {
                        const mv = new DataView(memory.buffer);
                        const opsPtr = mv.getUint32(slotsPtr, true);
                        const opsLen = mv.getUint32(slotsPtr + 4, true);
                        const opsBytes = new Uint8Array(memory.buffer, opsPtr, opsLen).slice();
                        wasm.free(opsPtr);
                        wasm.free(slotsPtr);
                        img.dataset.debug = `IMAGE bm=${bmIndex} pos=${imgPos} type=8 wmfops=${opsLen}bytes`;
                        renderWmfToBlob(opsBytes).then(blob => {
                            img.src = URL.createObjectURL(blob);
                            img.dataset.debug += ` png=${blob.size}bytes`;
                        }).catch(e => {
                            console.warn('wmf render bm' + bmIndex + ':', e);
                            img.alt = `[wmf:${bmIndex}]`;
                        });
                    } else {
                        const errPtr = wasm.hlp_get_error();
                        const err = errPtr ? readCString(errPtr) : 'unknown';
                        console.warn('wmf decode bm' + bmIndex + ':', err);
                        wasm.free(slotsPtr);
                        const ph = document.createElement('span');
                        ph.textContent = `[wmf:${bmIndex}]`;
                        ph.className = 'hlp-image-placeholder';
                        img.replaceWith(ph);
                    }
                } else {
                    const slotsPtr2 = wasm.malloc(8);
                    const rc2 = wasm.hlp_decode_image_png(currentHandle, bmIndex, slotsPtr2, slotsPtr2 + 4);
                    if (rc2 === 0) {
                        const mem2 = new DataView(memory.buffer);
                        const pngPtr = mem2.getUint32(slotsPtr2, true);
                        const pngLen = mem2.getUint32(slotsPtr2 + 4, true);
                        const pngData = new Uint8Array(memory.buffer, pngPtr, pngLen).slice();
                        wasm.free(slotsPtr2);

                        const blob = new Blob([pngData], { type: 'image/png' });
                        img.src = URL.createObjectURL(blob);
                        const szPtr = wasm.malloc(6);
                        wasm.hlp_get_last_image_size(szPtr, szPtr + 2, szPtr + 4);
                        const szView = new DataView(memory.buffer);
                        const dispW = szView.getUint16(szPtr, true);
                        const dispH = szView.getUint16(szPtr + 2, true);
                        const imgType = szView.getUint8(szPtr + 4);
                        wasm.free(szPtr);
                        img.dataset.debug = `IMAGE bm=${bmIndex} pos=${imgPos} type=${imgType} disp=${dispW}x${dispH} ${pngLen}bytes`;
                        img.onload = () => { img.dataset.debug += ` actual=${img.naturalWidth}x${img.naturalHeight}`; };
                        if (dispW && dispH) {
                            img.width = dispW;
                            img.height = dispH;
                        }
                    } else {
                        const errPtr = wasm.hlp_get_error();
                        const err = errPtr ? readCString(errPtr) : 'unknown';
                        console.warn('Image decode failed, bm' + bmIndex + ':', err);
                        wasm.free(slotsPtr2);
                        const ph = document.createElement('span');
                        ph.textContent = `[image:${bmIndex}]`;
                        ph.className = 'hlp-image-placeholder';
                        img.replaceWith(ph);
                    }
                }
                /* Don't reset currentSpan — font continues after image */
                break;
            }
            case 0x0A: { /* HOTSPOT_LINK */
                const hsKind = buf.getUint8(off++);
                const hsHash = buf.getUint32(off, true); off += 4;
                const hsX = buf.getUint16(off, true); off += 2;
                const hsY = buf.getUint16(off, true); off += 2;
                const hsW = buf.getUint16(off, true); off += 2;
                const hsH = buf.getUint16(off, true); off += 2;

                /* Find the preceding image and attach a <map> */
                const prevImg = currentPara && currentPara.querySelector('img:last-of-type');
                if (prevImg) {
                    let mapEl = prevImg.useMap
                        ? container.querySelector(`map[name="${prevImg.useMap.slice(1)}"]`)
                        : null;
                    if (!mapEl) {
                        const mapName = 'hlp-map-' + (++mapCounter);
                        mapEl = document.createElement('map');
                        mapEl.name = mapName;
                        prevImg.useMap = '#' + mapName;
                        prevImg.after(mapEl);
                    }
                    const area = document.createElement('area');
                    area.shape = 'rect';
                    area.coords = `${hsX},${hsY},${hsX + hsW},${hsY + hsH}`;
                    area.href = hsKind <= 1 && hsHash ? `#${currentFileKey}/topic-${hsHash}` : '#';
                    area.dataset.debug = `HOTSPOT kind=${hsKind} target=${hsHash} ${hsX},${hsY} ${hsW}x${hsH}`;
                    area.onclick = e => {
                        e.preventDefault();
                        if (hsKind <= 1 && hsHash) {
                            if (hsKind === 1) showPopup(hsHash, area);
                            else navigateTo(hsHash);
                        }
                    };
                    mapEl.appendChild(area);
                }
                break;
            }
            case 0x0B: { /* TABLE_START */
                const ncols = buf.getUint8(off++);
                const tblType = buf.getUint8(off++);
                const minWidth = buf.getInt16(off, true); off += 2;
                const trLeft = buf.getInt16(off, true); off += 2;
                const trGapH = buf.getInt16(off, true); off += 2;
                const colWidths = [];
                const colDebug = [];
                for (let c = 0; c < ncols; c++) {
                    const gap = buf.getInt16(off, true); off += 2;
                    const wid = buf.getInt16(off, true); off += 2;
                    const cellx = buf.getInt16(off, true); off += 2;
                    colWidths.push(cellx);
                    colDebug.push(`g=${gap} w=${wid} cx=${cellx}`);
                }
                const table = document.createElement('table');
                table.className = 'hlp-table';
                table.dataset.debug = `TABLE type=${tblType} cols=${ncols} trleft=${trLeft} trgaph=${trGapH} cols=[${colDebug.join(' | ')}]`;
                /* trgaph is half-gap — used as cell padding.
                   Also set border-spacing for visible gap between cell borders. */
                if (trGapH > 0) {
                    const gapPt = trGapH * hlpScale / 20;
                    table.dataset.gapPt = gapPt;
                    table.style.borderCollapse = 'separate';
                    table.style.borderSpacing = gapPt + 'pt 0';
                }
                /* Column widths from cumulative cellx positions.
                   Type 0,2 = variable width (percentages), Type 1,3 = fixed (points) */
                const isVariable = (tblType === 0 || tblType === 2);
                const colgroup = document.createElement('colgroup');
                let prevX = trLeft;
                const lastCellx = colWidths.length > 0 ? colWidths[colWidths.length - 1] : 1;
                for (let c = 0; c < ncols; c++) {
                    const col = document.createElement('col');
                    const cellx = colWidths[c];
                    if (isVariable && lastCellx > 0) {
                        col.style.width = ((cellx - prevX) / lastCellx * 100) + '%';
                    } else {
                        col.style.width = ((cellx - prevX) * hlpScale / 20) + 'pt';
                    }
                    prevX = cellx;
                    colgroup.appendChild(col);
                }
                table.appendChild(colgroup);

                if (isVariable) {
                    const ml = trLeft * hlpScale / 20;
                    if (ml > 0.5) {
                        table.style.marginLeft = ml + 'pt';
                        table.style.width = `calc(100% - ${ml}pt)`;
                    } else {
                        table.style.width = '100%';
                    }
                    if (minWidth > 0) table.style.minWidth = (minWidth * hlpScale / 20) + 'pt';
                } else {
                    /* Fixed tables: apply trleft + trgaph, and set min-width */
                    const netLeft = (trLeft + trGapH) * hlpScale / 20;
                    if (Math.abs(netLeft) > 0.5) table.style.marginLeft = netLeft + 'pt';
                    const lastCx = colWidths.length > 0 ? colWidths[colWidths.length - 1] : 0;
                    const totalWidth = (lastCx - trLeft) * hlpScale / 20;
                    if (totalWidth > 0) table.style.minWidth = totalWidth + 'pt';
                }
                /* Fixed tables: apply trleft + trgaph */
                const netLeft = (trLeft + trGapH) * hlpScale / 20;
                if (Math.abs(netLeft) > 0.5) table.style.marginLeft = netLeft + 'pt';
                container.appendChild(table);
                const tr = document.createElement('tr');
                table.appendChild(tr);
                currentPara = document.createElement('td');
                tr.appendChild(currentPara);
                currentSpan = null;
                needNewBlock = false;
                inTable = true;
                break;
            }
            case 0x0C: { /* TABLE_CELL */
                const tr = currentPara && currentPara.closest ? currentPara.closest('tr') : null;
                if (tr) {
                    currentPara = document.createElement('td');
                    tr.appendChild(currentPara);
                }
                currentSpan = null;
                needNewBlock = false;
                break;
            }
            case 0x0D: { /* TABLE_ROW_END */
                const tbl = container.querySelector('table.hlp-table:last-of-type');
                if (tbl) {
                    const tr2 = document.createElement('tr');
                    tbl.appendChild(tr2);
                    currentPara = document.createElement('td');
                    tr2.appendChild(currentPara);
                }
                currentSpan = null;
                needNewBlock = false;
                break;
            }
            case 0x0E: /* TABLE_END */
                currentPara = null;
                currentSpan = null;
                needNewBlock = true;
                inTable = false;
                break;
            case 0x0F: { /* MACRO */
                const mLen = buf.getUint16(off, true); off += 2;
                const macroStr = readString(ptr + off, mLen); off += mLen;
                const wireMacroLink = (a) => {
                    a.dataset.debug += ` macro="${macroStr}"`;
                    const target = resolveMacroTarget(macroStr);
                    if (target) {
                        a.href = `#${currentFileKey}/topic-${target.offset}`;
                        if (target.kind === 'popup') {
                            a.onclick = e => { e.preventDefault(); showPopup(target.offset, a); };
                        } else {
                            a.onclick = null;
                        }
                    } else {
                        a.onclick = e => { e.preventDefault(); executeMacro(macroStr); };
                    }
                };
                if (currentLink && currentLink.classList.contains('hlp-link-macro')) {
                    wireMacroLink(currentLink);
                } else if (currentPara) {
                    const lastArea = currentPara.querySelector('map:last-of-type area:last-of-type');
                    if (lastArea && lastArea.dataset.debug && lastArea.dataset.debug.includes('kind=2')) {
                        wireMacroLink(lastArea);
                        break;
                    }
                    pendingMacros.push(macroStr);
                } else {
                    pendingMacros.push(macroStr);
                }
                break;
            }
            case 0x10: { /* NON_SCROLL_START */
                const nsrDiv = document.createElement('div');
                nsrDiv.className = 'hlp-nsr';
                if (isRTL) nsrDiv.dir = 'rtl';
                container.appendChild(nsrDiv);
                currentPara = null;
                currentSpan = null;
                nsrParent = container;
                container = nsrDiv;
                break;
            }
            case 0x11: { /* NON_SCROLL_END */
                container = nsrParent || container.parentElement;
                currentPara = null;
                currentSpan = null;
                const srDiv = document.createElement('div');
                srDiv.className = 'hlp-sr';
                if (isRTL) srDiv.dir = 'rtl';
                container.appendChild(srDiv);
                container = srDiv;
                break;
            }
            case 0x12: /* NBSP */
                ensureSpan();
                currentSpan.appendChild(document.createTextNode('\u00A0'));
                break;
            case 0x13: /* NBHYPHEN */
                ensureSpan();
                currentSpan.appendChild(document.createTextNode('\u2011'));
                break;
            case 0x14: { /* ANCHOR */
                const anchorOff = buf.getUint32(off, true); off += 4;
                const anchor = document.createElement('a');
                anchor.id = 'topic-' + anchorOff;
                container.appendChild(anchor);
                break;
            }
            case 0xFF: /* PAGE_END */
                return { title, element: rootContainer, browseBwd, browseFwd, macros: pendingMacros };
            default:
                console.warn('Unknown opcode:', op.toString(16), 'at offset', off - 1);
                return { title, element: rootContainer, browseBwd, browseFwd, macros: pendingMacros };
            }
        }
        return { title, element: rootContainer, browseBwd, browseFwd, macros: pendingMacros };
    }

    /* WinHelp context string hash (base-43) */
    function hlpHash(str) {
        let hash = 0;
        for (let i = 0; i < str.length; i++) {
            const c = str[i];
            let x = 0;
            if (c >= 'A' && c <= 'Z') x = c.charCodeAt(0) - 65 + 17;
            else if (c >= 'a' && c <= 'z') x = c.charCodeAt(0) - 97 + 17;
            else if (c >= '1' && c <= '9') x = c.charCodeAt(0) - 48;
            else if (c === '0') x = 10;
            else if (c === '.') x = 12;
            else if (c === '_') x = 13;
            if (x) hash = (hash * 43 + x) | 0;
        }
        return hash;
    }

    function resolveHash(hash) {
        const offset = wasm.hlp_debug_lookup(hash >>> 0);
        return offset >= 0 ? offset : -1;
    }

    /* --- Macro system --- */

    function parseMacro(str) {
        /* Parse "Func(arg1, arg2); Func2()" into [{name, args}, ...] */
        const calls = [];
        let i = 0;
        while (i < str.length) {
            while (i < str.length && (str[i] === ' ' || str[i] === ';')) i++;
            if (i >= str.length) break;
            let nameStart = i;
            while (i < str.length && str[i] !== '(') i++;
            const name = str.substring(nameStart, i).trim();
            if (!name || i >= str.length) break;
            i++; /* skip ( */
            const args = [];
            let depth = 1;
            let argStart = i;
            let inString = false;
            while (i < str.length && depth > 0) {
                if (str[i] === '"') { inString = !inString; }
                else if (!inString) {
                    if (str[i] === '(') depth++;
                    else if (str[i] === ')') { depth--; if (depth === 0) break; }
                    else if (str[i] === ',' && depth === 1) {
                        args.push(str.substring(argStart, i).trim());
                        argStart = i + 1;
                    }
                }
                i++;
            }
            const lastArg = str.substring(argStart, i).trim();
            if (lastArg) args.push(lastArg);
            i++; /* skip ) */
            /* Strip quotes from string args */
            const cleanArgs = args.map(a =>
                a.startsWith('"') && a.endsWith('"') ? a.slice(1, -1) :
                a.startsWith('`') && a.endsWith("'") ? a.slice(1, -1) : a
            );
            calls.push({ name, args: cleanArgs });
        }
        return calls;
    }

    /* Look up a keyword (or K/A/context) without navigating. Returns offset or -1. */
    function resolveKeywordTarget(useALink, keyword, fallbackId) {
        if (!currentHandle || !keyword) return -1;
        const encoder = new TextEncoder();
        const kwBytes = encoder.encode(keyword + '\0');
        const kwPtr = wasm.malloc(kwBytes.length);
        new Uint8Array(memory.buffer).set(kwBytes, kwPtr);
        const offsetsPtr = wasm.malloc(64 * 4);
        const count = wasm.hlp_search_keyword(currentHandle, useALink, kwPtr, offsetsPtr, 64);
        const mem = new DataView(memory.buffer);
        let offset = -1;
        if (count === 1) offset = mem.getUint32(offsetsPtr, true);
        wasm.free(kwPtr);
        wasm.free(offsetsPtr);
        if (offset < 0 && count === 0) {
            offset = resolveHash(hlpHash(keyword));
            if (offset < 0 && fallbackId) offset = resolveHash(hlpHash(fallbackId));
        }
        return offset;
    }

    /* Try to pre-resolve a single-call macro to a topic offset. Returns
       { kind: 'jump'|'popup', offset } or null if it's a complex/unknown macro. */
    function resolveMacroTarget(str) {
        const calls = parseMacro(str);
        if (calls.length !== 1) return null;
        const { name, args } = calls[0];
        const n = name.toLowerCase();
        const tryNum = s => { const v = parseInt(s); return isNaN(v) ? -1 : v; };
        let kind, off = -1;
        switch (n) {
            case 'jumphash': case 'jh':
                kind = 'jump';
                off = resolveHash(tryNum(args.length >= 3 ? args[2] : args[1] || args[0]) >>> 0);
                break;
            case 'popuphash': case 'ph':
                kind = 'popup';
                off = resolveHash(tryNum(args.length >= 2 ? args[1] : args[0]) >>> 0);
                break;
            case 'jumpcontext': case 'jc': {
                kind = 'jump';
                const c = tryNum(args.length >= 3 ? args[2] : args[1] || args[0]);
                if (c >= 0) off = c;
                break;
            }
            case 'popupcontext': case 'pc':
                kind = 'popup';
                off = tryNum(args.length >= 2 ? args[1] : args[0]);
                break;
            case 'jumpid': case 'ji':
                kind = 'jump';
                off = resolveHash(hlpHash(args.length >= 2 ? args[1] : args[0]));
                break;
            case 'popupid': case 'pi':
                kind = 'popup';
                off = resolveHash(hlpHash(args.length >= 2 ? args[1] : args[0]));
                break;
            case 'alink': case 'al':
                kind = 'jump';
                off = resolveKeywordTarget(1, args[0], args[2]);
                break;
            case 'klink': case 'kl':
                kind = 'jump';
                off = resolveKeywordTarget(0, args[0], args[2]);
                break;
            case 'jumpcontents':
                kind = 'jump'; off = 0; break;
        }
        return off >= 0 ? { kind, offset: off } : null;
    }

    let macroLookup = null;
    function executeMacro(str) {
        if (!macroLookup) {
            macroLookup = {};
            for (const k of Object.keys(macroHandlers))
                macroLookup[k.toLowerCase()] = macroHandlers[k];
        }
        const calls = parseMacro(str);
        for (const { name, args } of calls) {
            const handler = macroLookup[name.toLowerCase()];
            if (handler) {
                handler(...args);
            } else {
                console.log('Unhandled macro:', name, args);
            }
        }
    }

    const macroHandlers = {
        BrowseButtons: () => { /* always shown */ },
        Contents: () => { navigateTo(0); },
        Exit: () => { /* no-op in browser */ },
        Back: () => { window.history.back(); },
        Prev: () => { window.history.back(); },
        Next: () => { window.history.forward(); },
        Print: () => { window.print(); },
        PlayWave: () => { /* no audio support */ },
        ExecFile: (file) => {
            /* Open URLs and mailto links in a new tab. File paths are ignored. */
            if (!file) return;
            if (/^(https?:\/\/|mailto:|ftp:\/\/)/i.test(file) ||
                /^[\w.+-]+@[\w.-]+\.\w+$/.test(file)) {
                const url = /@/.test(file) && !/^mailto:/i.test(file) ? 'mailto:' + file : file;
                window.open(url, '_blank', 'noopener');
            }
        },
        EF: (...a) => macroHandlers.ExecFile(...a),
        JumpHash: (...a) => {
            const h = parseInt(a.length >= 3 ? a[2] : a[1] || a[0]);
            if (!isNaN(h)) {
                const offset = wasm.hlp_debug_lookup(h >>> 0);
                if (offset >= 0) navigateTo(offset);
            }
        },
        JumpContext: (...a) => {
            const c = parseInt(a.length >= 3 ? a[2] : a[1] || a[0]);
            if (!isNaN(c)) navigateTo(c);
        },
        JumpID: (...a) => {
            const id = a.length >= 2 ? a[1] : a[0];
            if (id) {
                const offset = resolveHash(hlpHash(id));
                if (offset >= 0) navigateTo(offset);
            }
        },
        JumpContents: () => { navigateTo(0); },
        PopupHash: (...a) => {
            const h = parseInt(a.length >= 2 ? a[1] : a[0]);
            if (!isNaN(h)) {
                const offset = wasm.hlp_debug_lookup(h >>> 0);
                if (offset >= 0) showPopup(offset, null);
            }
        },
        PopupContext: (...a) => {
            const c = parseInt(a.length >= 2 ? a[1] : a[0]);
            if (!isNaN(c)) showPopup(c, null);
        },
        PopupId: (...a) => {
            const id = a.length >= 2 ? a[1] : a[0];
            if (id) {
                const offset = resolveHash(hlpHash(id));
                if (offset >= 0) showPopup(offset, null);
            }
        },
        CreateButton: (id, name, macro) => {
            const buttons = sticky.querySelector('.hlp-toolbar');
            if (!buttons) return;
            const btn = document.createElement('button');
            btn.textContent = name.replace('&', '');
            btn.id = 'hlp-btn-' + id;
            btn.onclick = () => executeMacro(macro);
            const browseBtn = document.getElementById('hlp-btn-BTN_PREV');
            buttons.insertBefore(btn, browseBtn);
        },
        DisableButton: (id) => {
            const btn = document.getElementById('hlp-btn-' + id);
            if (btn) btn.disabled = true;
        },
        EnableButton: (id) => {
            const btn = document.getElementById('hlp-btn-' + id);
            if (btn) btn.disabled = false;
        },
        DestroyButton: (id) => {
            const btn = document.getElementById('hlp-btn-' + id);
            if (btn) btn.remove();
        },
        SetContents: () => { /* store for Contents() call */ },
        ChangeButtonBinding: (id, macro) => {
            const btn = document.getElementById('hlp-btn-' + id);
            if (btn) btn.onclick = () => executeMacro(macro);
        },
        ALink: (...a) => { showKeywordResults(1, a[0], a[2]); },
        KLink: (...a) => { showKeywordResults(0, a[0], a[2]); },
        /* Aliases */
        AL: (...a) => macroHandlers.ALink(...a),
        KL: (...a) => macroHandlers.KLink(...a),
        CB: (...a) => macroHandlers.CreateButton(...a),
        DB: (...a) => macroHandlers.DisableButton(...a),
        EB: (...a) => macroHandlers.EnableButton(...a),
        JH: (...a) => macroHandlers.JumpHash(...a),
        JC: (...a) => macroHandlers.JumpContext(...a),
        JI: (...a) => macroHandlers.JumpID(...a),
        PC: (...a) => macroHandlers.PopupContext(...a),
        PI: (...a) => macroHandlers.PopupId(...a),
        CBB: (...a) => macroHandlers.ChangeButtonBinding(...a),
        CE: (...a) => { macroHandlers.ChangeButtonBinding(a[0], a[1]); macroHandlers.EnableButton(a[0]); },
        CS: () => { /* CloseSecondarys - no-op */ },
        CW: () => { /* CloseWindow - no-op */ },
        NS: () => { /* NoShow - no-op */ },
        FO: () => { /* FileOpen - no-op */ },
        ExecProgram: () => { /* can't execute programs from browser */ },
        ExecFile: (file) => {
            if (file && (file.startsWith('http://') || file.startsWith('https://')))
                window.open(file, '_blank');
        },
        SE: () => { /* ShellExecute - no-op */ },
        EF: (...a) => macroHandlers.ExecFile(...a),
        EP: () => { /* ExecProgram - no-op */ },
        IF: () => { /* IfThen - can't evaluate conditions */ },
        IE: () => { /* IfThenElse - can't evaluate conditions */ },
    };

    /* --- Navigation --- */
    let currentHandle = 0;
    let currentFileKey = null;
    let fileTitle = 'WinHelp Viewer';
    let hlpScale = 1;
    let startupMacros = [];
    let srColor = null;
    let nsrColor = null;
    let isRTL = false;
    let currentPageTopic = null;

    function dismissPopup() {
        const existing = document.getElementById('hlp-popup');
        if (existing) existing.remove();
    }

    function showPopup(topicOffset, anchorEl) {
        dismissPopup();
        const slotsPtr = wasm.malloc(8);
        const mem = new DataView(memory.buffer);
        const rc = wasm.hlp_render_page(currentHandle, topicOffset, slotsPtr, slotsPtr + 4);
        if (rc !== 0) { wasm.free(slotsPtr); return; }
        const dataPtr = mem.getUint32(slotsPtr, true);
        const dataLen = mem.getUint32(slotsPtr + 4, true);
        wasm.free(slotsPtr);

        const { element } = renderOpcodes(dataPtr, dataLen);

        const popup = document.createElement('div');
        popup.id = 'hlp-popup';
        popup.appendChild(element);

        document.body.appendChild(popup);

        /* Position near the anchor, clamped to viewport */
        const rect = anchorEl ? anchorEl.getBoundingClientRect() : { left: 100, bottom: 100 };
        let x = rect.left;
        let y = rect.bottom + 4;
        const pw = popup.offsetWidth;
        const ph = popup.offsetHeight;
        if (x + pw > window.innerWidth - 16) x = window.innerWidth - pw - 16;
        if (y + ph > window.innerHeight - 16) y = rect.top - ph - 4;
        if (x < 16) x = 16;
        if (y < 16) y = 16;
        popup.style.left = x + 'px';
        popup.style.top = y + 'px';

        /* Dismiss on click outside */
        setTimeout(() => {
            document.addEventListener('click', function dismiss(e) {
                if (!popup.contains(e.target)) {
                    popup.remove();
                    document.removeEventListener('click', dismiss);
                }
            });
        }, 0);
    }

    function showKeywordResults(useALink, keyword, fallbackId) {
        if (!currentHandle || !keyword) return;
        const encoder = new TextEncoder();
        const kwBytes = encoder.encode(keyword + '\0');
        const kwPtr = wasm.malloc(kwBytes.length);
        new Uint8Array(memory.buffer).set(kwBytes, kwPtr);
        const offsetsPtr = wasm.malloc(64 * 4);
        const count = wasm.hlp_search_keyword(currentHandle, useALink, kwPtr, offsetsPtr, 64);
        const mem = new DataView(memory.buffer);
        const offsets = [];
        for (let i = 0; i < count; i++)
            offsets.push(mem.getUint32(offsetsPtr + i * 4, true));
        wasm.free(kwPtr);
        wasm.free(offsetsPtr);

        if (offsets.length === 1) { navigateTo(offsets[0]); return; }
        if (offsets.length > 1) { showOffsetPicker(offsets); return; }
        /* Keyword not in A/K tree — fall back to context lookup, then fallbackId */
        let offset = resolveHash(hlpHash(keyword));
        if (offset < 0 && fallbackId) offset = resolveHash(hlpHash(fallbackId));
        if (offset >= 0) navigateTo(offset);
    }

    function renderPage(topicOffset) {
        currentPageTopic = getPageTopic(topicOffset) || topicOffset;
        dismissPopup();
        const slotsPtr = wasm.malloc(8);
        const mem = new DataView(memory.buffer);

        const rc = wasm.hlp_render_page(currentHandle, topicOffset, slotsPtr, slotsPtr + 4);
        if (rc !== 0) {
            const errPtr = wasm.hlp_get_error();
            console.error('hlp_render_page failed:', errPtr ? readCString(errPtr) : 'unknown', 'topic:', topicOffset);
            wasm.free(slotsPtr);
            return;
        }

        const dataPtr = mem.getUint32(slotsPtr, true);
        const dataLen = mem.getUint32(slotsPtr + 4, true);
        wasm.free(slotsPtr);

        const { title, element, browseBwd, browseFwd, macros } = renderOpcodes(dataPtr, dataLen);

        const header = document.createElement('div');
        header.className = 'hlp-header';
        const headerTitle = document.createElement('span');
        headerTitle.className = 'hlp-header-file';
        headerTitle.textContent = fileTitle;
        header.appendChild(headerTitle);
        if (title) {
            const sep = document.createElement('span');
            sep.textContent = ' \u2014 ';
            header.appendChild(sep);
            const pageTitle = document.createElement('span');
            pageTitle.className = 'hlp-header-page';
            pageTitle.textContent = title;
            header.appendChild(pageTitle);
        }

        const toolbar = document.createElement('div');
        toolbar.className = 'hlp-toolbar';

        const contBtn = document.createElement('button');
        contBtn.textContent = 'Contents';
        contBtn.onclick = () => navigateTo(0);
        toolbar.appendChild(contBtn);

        const idxBtn = document.createElement('button');
        idxBtn.textContent = 'Index';
        idxBtn.onclick = () => {
            history.replaceState({ scrollY: window.scrollY }, '', location.href);
            location.hash = currentFileKey + '/index';
        };
        toolbar.appendChild(idxBtn);

        const backBtn = document.createElement('button');
        backBtn.textContent = 'Back';
        backBtn.id = 'hlp-btn-back';
        if (window.navigation) backBtn.disabled = !navigation.canGoBack;
        backBtn.onclick = () => window.history.back();
        toolbar.appendChild(backBtn);

        const fwdBtn = document.createElement('button');
        fwdBtn.textContent = 'Forward';
        fwdBtn.id = 'hlp-btn-fwd';
        if (window.navigation) fwdBtn.disabled = !navigation.canGoForward;
        fwdBtn.onclick = () => window.history.forward();
        toolbar.appendChild(fwdBtn);

        /* Startup macros insert buttons here (e.g. CreateButton) */

        const prevBtn = document.createElement('button');
        prevBtn.textContent = '<<';
        prevBtn.id = 'hlp-btn-BTN_PREV';
        prevBtn.disabled = browseBwd === 0xFFFFFFFF;
        prevBtn.onclick = () => navigateTo(browseBwd);
        toolbar.appendChild(prevBtn);

        const nextBtn = document.createElement('button');
        nextBtn.textContent = '>>';
        nextBtn.id = 'hlp-btn-BTN_NEXT';
        nextBtn.disabled = browseFwd === 0xFFFFFFFF;
        nextBtn.onclick = () => navigateTo(browseFwd);
        toolbar.appendChild(nextBtn);

        const printBtn = document.createElement('button');
        printBtn.textContent = 'Print';
        printBtn.onclick = () => window.print();
        toolbar.appendChild(printBtn);

        const topBtn = document.createElement('button');
        topBtn.textContent = 'Top';
        topBtn.className = 'hlp-toolbar-right';
        topBtn.onclick = () => window.scrollTo(0, 0);
        toolbar.appendChild(topBtn);

        /* Sticky block: header, toolbar, NSR (after #app) */
        /* Remove previous header/toolbar/nsr, keep #app */
        while (sticky.lastElementChild !== app) sticky.lastElementChild.remove();
        sticky.appendChild(header);
        sticky.appendChild(toolbar);

        const nsr = element.querySelector('.hlp-nsr');
        if (nsr) {
            sticky.appendChild(nsr);
            if (nsrColor) nsr.style.backgroundColor = nsrColor;
        }

        /* Scrolling content */
        content.innerHTML = '';
        content.style.backgroundColor = srColor || '';
        const sr = element.querySelector('.hlp-sr');
        if (sr) {
            content.appendChild(sr);
        } else {
            content.appendChild(element);
        }

        document.title = title ? `${fileTitle} - ${title}` : fileTitle;

        /* Execute page-load macros */
        for (const m of startupMacros) executeMacro(m);
        for (const m of macros) executeMacro(m);
    }

    function parseHash() {
        const h = location.hash.slice(1);
        if (!h) return { file: null, page: null, topic: 0 };
        const slash = h.indexOf('/');
        if (slash < 0) return { file: h, page: null, topic: 0 };
        const file = h.slice(0, slash);
        const rest = h.slice(slash + 1);
        if (rest === 'topics' || rest === 'index') return { file, page: 'index', topic: 0, query: '' };
        if (rest.startsWith('index/')) return { file, page: 'index', topic: 0, query: decodeURIComponent(rest.slice(6)) };
        const topic = rest.startsWith('topic-') ? parseInt(rest.slice(6)) : 0;
        return { file, page: null, topic, query: '' };
    }

    function fileBaseName(name) {
        return name.replace(/\.hlp$/i, '');
    }

    function getPageTopic(offset) {
        const idx = wasm.hlp_find_page(offset);
        return idx >= 0 ? wasm.hlp_get_page_topic_offset(idx) : 0;
    }

    function updateScrollPadding() {
        document.documentElement.style.scrollPaddingTop = sticky.offsetHeight + 'px';
    }

    /* Find element by exact id, or the paragraph whose range contains the offset */
    function findTopicElement(offset) {
        const el = document.getElementById('topic-' + offset);
        if (el) return el;
        let best = null;
        const paras = content.querySelectorAll('.hlp-para[id^="topic-"]');
        for (const p of paras) {
            if (parseInt(p.id.slice(6)) <= offset) best = p;
            else break;
        }
        return best || paras[0] || null;
    }

    function updateNavButtons() {
        if (!window.navigation) return;
        const back = document.getElementById('hlp-btn-back');
        const fwd = document.getElementById('hlp-btn-fwd');
        if (back) back.disabled = !navigation.canGoBack;
        if (fwd) fwd.disabled = !navigation.canGoForward;
    }

    function navigateTo(offset) {
        history.replaceState({ scrollY: window.scrollY }, '', location.href);

        const pageTopic = getPageTopic(offset);
        if (pageTopic !== currentPageTopic) {
            renderPage(pageTopic);
            updateScrollPadding();
        }
        const base = currentFileKey;
        if (!offset) {
            window.scrollTo(0, 0);
            location.hash = base;
        } else {
            const el = findTopicElement(offset);
            if (el) {
                location.hash = base + '/' + el.id;
                el.scrollIntoView();
            }
        }
        updateNavButtons();
    }

    let _popstateHandled = false;

    window.addEventListener('popstate', async e => {
        _popstateHandled = true;
        const state = e.state || {};
        const { file, page, topic, query } = parseHash();
        /* Switch file if hash references a different one */
        if (file && file !== (currentFileKey || '')) {
            const entry = await loadFileByKey(file);
            if (entry) {
                openBytes(new Uint8Array(entry.bytes), entry.name, topic);
                if (page === 'index') renderIndexPage(query);
                else if (state.scrollY != null) {
                    requestAnimationFrame(() => window.scrollTo(0, state.scrollY));
                }
                updateNavButtons();
                saveLastHash();
                return;
            }
        }
        if (!currentHandle) { updateNavButtons(); return; }
        if (page === 'index') {
            renderIndexPage(query);
        } else {
            const pageTopic = getPageTopic(topic);
            if (pageTopic !== currentPageTopic) {
                renderPage(pageTopic);
                updateScrollPadding();
            }
        }
        updateNavButtons();
        if (!page && state.scrollY != null) {
            requestAnimationFrame(() => window.scrollTo(0, state.scrollY));
        }
        saveLastHash();
    });

    window.addEventListener('hashchange', async () => {
        /* Skip if popstate already handled this navigation */
        if (_popstateHandled) { _popstateHandled = false; return; }
        const { file, page, topic, query } = parseHash();
        if (file && file !== (currentFileKey || '')) {
            const entry = await loadFileByKey(file);
            if (entry) {
                openBytes(new Uint8Array(entry.bytes), entry.name, topic);
                if (page === 'index') renderIndexPage(query);
                updateNavButtons();
                saveLastHash();
                return;
            }
        }
        if (!currentHandle) return;
        if (page === 'index') {
            renderIndexPage(query);
        } else {
            const pageTopic = getPageTopic(topic);
            if (pageTopic === currentPageTopic) { updateNavButtons(); saveLastHash(); return; }
            renderPage(pageTopic);
            updateScrollPadding();
        }
        updateNavButtons();
        saveLastHash();
    });

    async function saveLastHash() {
        if (!currentFileKey) return;
        const h = location.hash.slice(1);
        const slash = h.indexOf('/');
        const suffix = slash >= 0 ? h.slice(slash) : '';
        try {
            const db = await openDB();
            const tx = db.transaction('files', 'readwrite');
            const files = tx.objectStore('files');
            const getReq = files.get(currentFileKey);
            await new Promise(resolve => {
                getReq.onsuccess = () => {
                    const entry = getReq.result;
                    if (entry) {
                        entry.lastHash = suffix;
                        files.put(entry, currentFileKey);
                    }
                    resolve();
                };
                getReq.onerror = resolve;
            });
            db.close();
        } catch {}
    }

    function openBytes(bytes, name, topicOffset) {
        currentFileKey = fileBaseName(name);
        currentHandle = 0;
        syncFileSelect();

        const needed = bytes.length * 4 + 1024 * 1024; /* file x4 + 1MB overhead */
        const currentBytes = memory.buffer.byteLength;
        if (needed > currentBytes) {
            const pages = Math.ceil((needed - currentBytes) / 65536);
            memory.grow(pages);
        }
        wasm.hlp_init();

        const ptr = wasm.malloc(bytes.length);
        if (!ptr) { content.textContent = 'Error: malloc failed'; return; }
        new Uint8Array(memory.buffer).set(bytes, ptr);

        currentHandle = wasm.hlp_open(ptr, bytes.length);
        if (!currentHandle) { showError(); return; }

        const infoPtr = wasm.hlp_get_info(currentHandle);

        /* Set text decoder based on charset before reading any strings */
        const infoView = new DataView(memory.buffer);
        let charset = infoView.getUint16(infoPtr + 262, true);
        const lcid = infoView.getUint16(infoPtr + 268, true);
        const charsetMap = {
            0: 'windows-1252', 1: 'windows-1252',
            128: 'shift_jis', 129: 'euc-kr', 134: 'gb2312', 136: 'big5',
            161: 'windows-1253', 162: 'windows-1254', 177: 'windows-1255',
            178: 'windows-1256', 186: 'windows-1257', 204: 'windows-1251',
            238: 'windows-1250',
        };
        /* RTL for Arabic/Hebrew content */
        const rtlCharsets = new Set([177, 178]);
        const rtlLangs = new Set([0x01, 0x0d, 0x20, 0x29]);
        isRTL = rtlCharsets.has(charset) || rtlLangs.has(lcid & 0xff);
        /* If charset is 0/1 (default), derive from LCID primary language */
        if ((charset === 0 || charset === 1) && lcid) {
            const lcidToEncoding = {
                0x01: 'windows-1256', 0x0d: 'windows-1255', 0x11: 'shift_jis',
                0x12: 'euc-kr', 0x19: 'windows-1251', 0x1e: 'windows-874',
                0x1f: 'windows-1254', 0x42: 'windows-1257', 0x2a: 'windows-1258',
                0x05: 'windows-1250', 0x08: 'windows-1253', 0x1a: 'windows-1250',
                0x24: 'windows-1250',
            };
            const enc = lcidToEncoding[lcid & 0xff];
            if (enc) decoder = new TextDecoder(enc);
            else decoder = new TextDecoder(charsetMap[charset] || 'windows-1252');
        } else {
            decoder = new TextDecoder(charsetMap[charset] || 'windows-1252');
        }
        /* scale: 10 for old fonts (values in half-points), 1 for new (twips) */
        hlpScale = infoView.getUint16(infoPtr + 266, true) || 1;

        fileTitle = readCString(infoPtr) || name;

        /* Read region background colors */
        const rawBytes = new Uint8Array(memory.buffer);
        srColor = rawBytes[infoPtr + 273]
            ? `rgb(${rawBytes[infoPtr + 270]},${rawBytes[infoPtr + 271]},${rawBytes[infoPtr + 272]})`
            : null;
        nsrColor = rawBytes[infoPtr + 277]
            ? `rgb(${rawBytes[infoPtr + 274]},${rawBytes[infoPtr + 275]},${rawBytes[infoPtr + 276]})`
            : null;

        buildFontStyles(currentHandle);

        /* Read startup macros from |SYSTEM type 4 records */
        startupMacros = [];
        const lenSlot = wasm.malloc(4);
        const macroPtr = wasm.hlp_get_startup_macros(lenSlot);
        const macroLen = new DataView(memory.buffer).getUint32(lenSlot, true);
        wasm.free(lenSlot);
        if (macroPtr && macroLen > 0) {
            const macroBytes = new Uint8Array(memory.buffer, macroPtr, macroLen);
            let start = 0;
            for (let i = 0; i < macroLen; i++) {
                if (macroBytes[i] === 0) {
                    if (i > start) startupMacros.push(decoder.decode(macroBytes.subarray(start, i)));
                    start = i + 1;
                }
            }
        }

        const { file: hashFile, page: hashPage, topic: hashTopic, query: hashQuery } = parseHash();
        const base = fileBaseName(name);
        const isThisFile = hashFile === base;
        const startTopic = (isThisFile ? hashTopic : 0) || topicOffset || 0;
        const pageTopic = startTopic ? getPageTopic(startTopic) : 0;
        renderPage(pageTopic || startTopic);
        updateScrollPadding();

        if (isThisFile && hashPage === 'index') {
            renderIndexPage(hashQuery);
        } else if (startTopic) {
            const el = findTopicElement(startTopic);
            if (el) {
                location.hash = base + '/' + el.id;
                el.scrollIntoView();
            } else {
                location.hash = base;
            }
        } else {
            location.hash = base;
        }
        resetBtn.disabled = false;
    }

    /* IndexedDB helpers */
    async function openDB() {
        return new Promise((resolve, reject) => {
            const req = indexedDB.open('winhelp', 1);
            req.onupgradeneeded = () => {
                const db = req.result;
                if (!db.objectStoreNames.contains('files'))
                    db.createObjectStore('files');
                if (!db.objectStoreNames.contains('settings'))
                    db.createObjectStore('settings');
            };
            req.onsuccess = () => resolve(req.result);
            req.onerror = () => reject(req.error);
        });
    }

    async function cacheFile(name, bytes) {
        const db = await openDB();
        const key = fileBaseName(name);
        const tx = db.transaction(['files', 'settings'], 'readwrite');
        /* Preserve existing lastHash if we're re-caching */
        const files = tx.objectStore('files');
        await new Promise(resolve => {
            const getReq = files.get(key);
            getReq.onsuccess = () => {
                const lastHash = getReq.result?.lastHash || '';
                files.put({ name, bytes, lastHash }, key);
                resolve();
            };
            getReq.onerror = resolve;
        });
        tx.objectStore('settings').put(key, 'current');
        db.close();
    }

    async function loadFileByKey(key) {
        try {
            const db = await openDB();
            const entry = await new Promise((resolve, reject) => {
                const req = db.transaction('files', 'readonly').objectStore('files').get(key);
                req.onsuccess = () => resolve(req.result);
                req.onerror = () => reject(req.error);
            });
            db.close();
            return entry || null;
        } catch { return null; }
    }

    async function listCachedFiles() {
        try {
            const db = await openDB();
            const items = await new Promise((resolve, reject) => {
                const results = [];
                const tx = db.transaction('files', 'readonly');
                const req = tx.objectStore('files').openCursor();
                req.onsuccess = () => {
                    const cur = req.result;
                    if (cur) { results.push({ key: cur.key, name: cur.value.name }); cur.continue(); }
                    else resolve(results);
                };
                req.onerror = () => reject(req.error);
            });
            db.close();
            return items;
        } catch { return []; }
    }

    async function clearAllCache() {
        return new Promise((resolve) => {
            const req = indexedDB.deleteDatabase('winhelp');
            req.onsuccess = req.onerror = req.onblocked = () => resolve();
        });
    }

    async function setSetting(key, value) {
        try {
            const db = await openDB();
            const tx = db.transaction('settings', 'readwrite');
            tx.objectStore('settings').put(value, key);
            db.close();
        } catch {}
    }

    async function getSetting(key) {
        try {
            const db = await openDB();
            const val = await new Promise((resolve, reject) => {
                const req = db.transaction('settings', 'readonly').objectStore('settings').get(key);
                req.onsuccess = () => resolve(req.result);
                req.onerror = () => reject(req.error);
            });
            db.close();
            return val || null;
        } catch { return null; }
    }

    async function loadCachedFile() {
        const { file, topic } = parseHash();
        let key = file;
        if (!key) {
            key = await getSetting('current');
            if (!key) return false;
            const entry = await loadFileByKey(key);
            if (!entry) return false;
            location.hash = key + (entry.lastHash || '');
            /* location.hash set; hashchange handler will load the file */
            return true;
        }
        const entry = await loadFileByKey(key);
        if (!entry) return false;
        openBytes(new Uint8Array(entry.bytes), entry.name, topic);
        saveLastHash();
        return true;
    }

    async function openFile(file) {
        if (!file.name.match(/\.hlp$/i)) {
            alert('Please select a .HLP file');
            return;
        }
        const bytes = new Uint8Array(await file.arrayBuffer());
        openBytes(bytes, file.name, 0);
        await cacheFile(file.name, bytes.buffer);
        refreshFileSelect();
    }

    /* File picker */
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.hlp';
    input.onchange = () => { if (input.files[0]) openFile(input.files[0]); input.value = ''; };

    const btn = document.createElement('button');
    btn.textContent = 'Open .hlp file';
    btn.onclick = () => input.click();
    app.appendChild(btn);

    function resetToWelcome() {
        currentHandle = 0;
        currentFileKey = null;
        document.title = 'WinHelp Viewer';
        location.hash = '';
        content.style.backgroundColor = '';
        while (sticky.lastElementChild !== app) sticky.lastElementChild.remove();
        showWelcome();
        resetBtn.disabled = true;
        syncFileSelect();
    }

    const resetBtn = document.createElement('button');
    resetBtn.textContent = 'Reset';
    resetBtn.disabled = true;
    resetBtn.onclick = async () => {
        resetToWelcome();
        try {
            const db = await openDB();
            const tx = db.transaction('settings', 'readwrite');
            tx.objectStore('settings').delete('current');
            db.close();
        } catch {}
    };
    app.appendChild(resetBtn);

    const fileSelect = document.createElement('select');
    fileSelect.className = 'app-spacer';
    fileSelect.onchange = async () => {
        const key = fileSelect.value;
        if (!key || key === currentFileKey) return;
        const entry = await loadFileByKey(key);
        const saved = entry?.lastHash || '';
        location.hash = key + saved;
        setSetting('current', key);
    };
    app.appendChild(fileSelect);

    function syncFileSelect() {
        fileSelect.value = currentFileKey || '';
    }

    async function refreshFileSelect() {
        const items = await listCachedFiles();
        items.sort((a, b) => a.name.toLowerCase() < b.name.toLowerCase() ? -1 : 1);
        fileSelect.innerHTML = '';
        const placeholder = document.createElement('option');
        placeholder.value = '';
        placeholder.disabled = true;
        placeholder.textContent = items.length ? `Cached files (${items.length})` : 'No cached files';
        fileSelect.appendChild(placeholder);
        for (const it of items) {
            const opt = document.createElement('option');
            opt.value = it.key;
            opt.textContent = it.name;
            fileSelect.appendChild(opt);
        }
        fileSelect.disabled = !items.length;
        clearBtn.disabled = !items.length;
        syncFileSelect();
    }

    const clearBtn = document.createElement('button');
    clearBtn.textContent = 'Clear cache';
    clearBtn.onclick = async () => {
        if (!confirm('Clear all cached files? This cannot be undone.')) return;
        resetToWelcome();
        try { await clearAllCache(); } catch {}
        refreshFileSelect();
    };
    app.appendChild(clearBtn);

    refreshFileSelect();

    function setupListPage(label, placeholder) {
        if (!currentHandle) return null;
        currentPageTopic = -1;

        if (!sticky.querySelector('.hlp-toolbar')) {
            renderPage(0);
            updateScrollPadding();
        }
        const oldNsr = sticky.querySelector('.hlp-nsr');
        if (oldNsr) oldNsr.remove();

        content.innerHTML = '';
        content.style.backgroundColor = '#fff';
        document.title = `${fileTitle} - ${label}`;

        /* Search field in sticky NSR area */
        const nsr = document.createElement('div');
        nsr.className = 'hlp-nsr';
        const filterWrap = document.createElement('div');
        filterWrap.className = 'hlp-index-filter-wrap';
        const filterInput = document.createElement('input');
        filterInput.type = 'text';
        filterInput.placeholder = placeholder;
        filterInput.className = 'hlp-index-filter';
        const clearBtn = document.createElement('button');
        clearBtn.className = 'hlp-index-clear';
        clearBtn.textContent = '\u00D7';
        clearBtn.onclick = () => {
            filterInput.value = '';
            for (const item of content.querySelectorAll('.hlp-topics-item'))
                item.style.display = '';
            history.pushState(null, '', '#' + currentFileKey + '/index');
            filterInput.focus();
        };
        filterWrap.appendChild(filterInput);
        filterWrap.appendChild(clearBtn);
        nsr.appendChild(filterWrap);
        sticky.appendChild(nsr);
        updateScrollPadding();

        return filterInput;
    }

    function addListItem(text, iconSrc, filterKey, onclick) {
        const item = document.createElement('div');
        item.className = 'hlp-topics-item';
        item.dataset.kw = filterKey;
        const icon = document.createElement('img');
        icon.src = iconSrc;
        icon.className = 'hlp-topics-icon';
        item.appendChild(icon);
        const a = document.createElement('a');
        a.href = '#';
        a.textContent = text;
        a.onclick = e => { e.preventDefault(); onclick(); };
        item.appendChild(a);
        content.appendChild(item);
    }

    function renderIndexPage(query) {
        const filterInput = setupListPage('Index', 'Type a keyword...');
        if (!filterInput) return;

        /* Collect keywords, resolve their offsets for dedup */
        const kwCount = wasm.hlp_get_keyword_count(currentHandle);
        const seenOffsets = new Set();
        const entries = [];
        const offsetsPtr = wasm.malloc(64 * 4);

        if (kwCount > 0) {
            const ptrBuf = wasm.malloc(kwCount * 4);
            const actual = wasm.hlp_enum_keywords(currentHandle, ptrBuf, kwCount);
            const mem = new DataView(memory.buffer);
            for (let i = 0; i < actual; i++) {
                const rawPtr = mem.getUint32(ptrBuf + i * 4, true);
                const text = readCString(rawPtr);
                const cnt = wasm.hlp_search_keyword(currentHandle, 0, rawPtr, offsetsPtr, 64);
                if (cnt === 0) continue;
                const dv = new DataView(memory.buffer);
                const offsets = [];
                for (let j = 0; j < cnt; j++) {
                    const o = dv.getUint32(offsetsPtr + j * 4, true);
                    offsets.push(o);
                    seenOffsets.add(o);
                }
                if (offsets.length === 1) {
                    const off = offsets[0];
                    entries.push({ text, key: text.toLowerCase(),
                        onclick: () => navigateTo(off) });
                } else {
                    entries.push({ text, key: text.toLowerCase(),
                        onclick: () => showOffsetPicker(offsets) });
                }
            }
            wasm.free(ptrBuf);
        }

        /* Add topic titles whose offsets aren't covered by keywords */
        const topicCount = wasm.hlp_get_page_count();
        for (let i = 0; i < topicCount; i++) {
            const titlePtr = wasm.hlp_get_page_title(i);
            const title = titlePtr ? readCString(titlePtr) : '';
            if (!title) continue;
            const offset = wasm.hlp_get_page_topic_offset(i);
            if (seenOffsets.has(offset)) continue;
            seenOffsets.add(offset);
            entries.push({ text: title, key: title.toLowerCase(),
                onclick: () => navigateTo(offset) });
        }

        wasm.free(offsetsPtr);

        entries.sort((a, b) => a.key < b.key ? -1 : a.key > b.key ? 1 : 0);

        const headerPage = sticky.querySelector('.hlp-header-page');
        if (headerPage) headerPage.textContent = `${entries.length} entries`;

        for (const e of entries) {
            addListItem(e.text, 'icons/topic.png', e.key, e.onclick);
        }

        function applyFilter(val) {
            const lf = val.toLowerCase();
            for (const item of content.querySelectorAll('.hlp-topics-item')) {
                item.style.display = item.dataset.kw.includes(lf) ? '' : 'none';
            }
            const hashSuffix = val ? '/index/' + encodeURIComponent(val) : '/index';
            history.replaceState(null, '', '#' + currentFileKey + hashSuffix);
        }

        filterInput.addEventListener('input', () => applyFilter(filterInput.value));

        if (query) {
            filterInput.value = query;
            applyFilter(query);
        }

        filterInput.focus();
        window.scrollTo(0, 0);
    }

    function showOffsetPicker(offsets) {
        dismissPopup();
        const popup = document.createElement('div');
        popup.id = 'hlp-popup';

        for (const offset of offsets) {
            const pageIdx = wasm.hlp_find_page(offset);
            const titlePtr = pageIdx >= 0 ? wasm.hlp_get_page_title(pageIdx) : 0;
            const title = titlePtr ? readCString(titlePtr) : `Topic at ${offset}`;
            const a = document.createElement('a');
            a.href = `#${currentFileKey}/topic-${offset}`;
            a.textContent = title;
            a.style.display = 'block';
            a.onclick = e => { e.preventDefault(); dismissPopup(); navigateTo(offset); };
            popup.appendChild(a);
        }

        popup.style.left = '100px';
        popup.style.top = '100px';
        document.body.appendChild(popup);

        setTimeout(() => {
            document.addEventListener('click', function dismiss(e) {
                if (!popup.contains(e.target)) {
                    popup.remove();
                    document.removeEventListener('click', dismiss);
                }
            });
        }, 0);
    }

    /* Drag and drop */
    document.body.addEventListener('dragover', e => {
        e.preventDefault();
        content.classList.add('hlp-dragover');
    });
    document.body.addEventListener('dragleave', () => {
        content.classList.remove('hlp-dragover');
    });
    document.body.addEventListener('drop', e => {
        e.preventDefault();
        content.classList.remove('hlp-dragover');
        if (e.dataTransfer.files[0]) openFile(e.dataTransfer.files[0]);
    });

    function showWelcome() {
        content.innerHTML = '';
        content.style.backgroundColor = 'rgb(255, 255, 226)';
        const welcome = document.createElement('div');
        welcome.className = 'hlp-welcome';
        welcome.innerHTML =
            '<h1><img src="icons/hlpfile.png" class="hlp-welcome-icon" alt="">WinHelp</h1>' +
            '<p>A browser-based viewer for legacy Windows .HLP files.</p>' +
            '<p>Open a file using the button, or drag and drop a .hlp file here.</p>';
        const openBtn = document.createElement('button');
        openBtn.textContent = 'Open .hlp file';
        openBtn.className = 'hlp-welcome-btn';
        openBtn.onclick = () => input.click();
        welcome.appendChild(openBtn);
        content.appendChild(welcome);
    }

    /* Restore cached file on reload, or show welcome */
    if (!(await loadCachedFile())) showWelcome();
})();
