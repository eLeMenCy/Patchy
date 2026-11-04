// TidyLayout.ts — v0.0.966: ⌥⌘L "Tidy layout" (user's idea, 2026-11-03).
//
// Left → right in signal order:
//   • items = what's on screen as one box (a node, a folded wrap, an open
//     wrap's frame); links = wires between them (an item's own inner wires
//     are not links)
//   • each group of linked items (a component) is laid out on its own,
//     components stacked top to bottom (in their current top-to-bottom order)
//   • columns: longest path from the sources (feedback loops are broken where
//     a depth-first walk finds them); order in a column: start from the
//     current y, then a few barycentre sweeps (fewer crossings)
//   • items with no wires stay where they are — moved to the nearest free
//     spot only if the new layout now covers them (user, 2026-11-04)
//   • v0.0.967: a wire's port position counts in that order (a node's top
//     output feeds the upper of two monitors: no crossing); "fixed" items
//     (the Enter view's port rails, one per port) take part in the order and
//     the columns but are never moved and take no room
//   • the whole result starts at the laid-out items' current top-left, so
//     the graph stays where you were looking; on the grid when snap is on

export interface TidyItem { id: string; x: number; y: number; w: number; h: number; fixed?: boolean }
/** from, to, and where the wire sits on each (0 = top … 1 = bottom of the item) */
export type TidyLink = [string, string, number?, number?];
export interface TidyOptions { gapX?: number; gapY?: number; gapGroup?: number; grid?: number }

export function tidyLayout (items: TidyItem[], links: TidyLink[], opt: TidyOptions = {}): Map<string, { x: number; y: number }> {
  const gapX = opt.gapX ?? 96, gapY = opt.gapY ?? 40, gapGroup = opt.gapGroup ?? 96;
  const snap = (v: number) => opt.grid && opt.grid > 1 ? Math.round (v / opt.grid) * opt.grid : Math.round (v);
  const byId = new Map (items.map (i => [i.id, i]));
  const out  = new Map<string, { x: number; y: number }> ();

  // links: known items, no self links, no duplicates
  const succ = new Map<string, Set<string>> (), pred = new Map<string, Set<string>> ();
  // port offsets per link: "a>b" → [offset on a, offset on b] (several wires: averaged)
  const offs = new Map<string, { a: number; b: number; n: number }> ();
  for (const i of items) { succ.set (i.id, new Set()); pred.set (i.id, new Set()); }
  for (const [a, b, ao, bo] of links) {
    if (a === b || ! byId.has (a) || ! byId.has (b)) continue;
    succ.get (a)!.add (b); pred.get (b)!.add (a);
    const k = a + '>' + b, o = offs.get (k) ?? { a: 0, b: 0, n: 0 };
    o.a += clamp01 (ao ?? 0.5); o.b += clamp01 (bo ?? 0.5); o.n++;
    offs.set (k, o);
  }
  const offOn = (from: string, to: string, side: 'a' | 'b') => { const o = offs.get (from + '>' + to); return o ? o[side] / o.n : 0.5; };
  const linked   = items.filter (i => succ.get (i.id)!.size + pred.get (i.id)!.size > 0);
  const isolated = items.filter (i => succ.get (i.id)!.size + pred.get (i.id)!.size === 0);
  if (linked.length === 0) return out;

  // components (undirected)
  const fixedLinked = linked.filter (i => i.fixed).map (i => i.id);
  const comp = new Map<string, number> ();
  const comps: TidyItem[][] = [];
  for (const i of linked) {
    if (comp.has (i.id)) continue;
    const k = comps.length, list: TidyItem[] = [], stack = [i.id];
    comp.set (i.id, k);
    while (stack.length) {
      const id = stack.pop()!;
      list.push (byId.get (id)!);
      // fixed items (the rails of one Enter view) hold everything they touch together
      const extra = byId.get (id)!.fixed ? fixedLinked : [];
      for (const n of [...succ.get (id)!, ...pred.get (id)!, ...extra])
        if (! comp.has (n)) { comp.set (n, k); stack.push (n); }
    }
    comps.push (list);
  }
  const topOf = (c: TidyItem[]) => Math.min (...c.filter (i => ! i.fixed).map (i => i.y));
  comps.sort ((a, b) => topOf (a) - topOf (b));

  const movable = linked.filter (i => ! i.fixed);
  if (movable.length === 0) return out;
  const originX = Math.min (...movable.map (i => i.x));
  let   cursorY = Math.min (...movable.map (i => i.y));
  const placed: { x: number; y: number; w: number; h: number }[] = [];

  for (const list of comps) {
    const ids = new Set (list.map (i => i.id));
    // break feedback loops: drop the back edges of a DFS (sources first, by y)
    const fwd = new Map<string, string[]> ();
    for (const id of ids) fwd.set (id, []);
    const state = new Map<string, 0 | 1 | 2> ();
    const roots = [...list].sort ((a, b) => (pred.get (a.id)!.size - pred.get (b.id)!.size) || (a.x - b.x) || (a.y - b.y));
    const visit = (id: string) => {
      state.set (id, 1);
      const next = [...succ.get (id)!].sort ((a, b) => byId.get (a)!.y - byId.get (b)!.y);
      for (const n of next) {
        const s = state.get (n) ?? 0;
        if (s === 1) continue;               // back edge: a loop — ignored for the columns
        fwd.get (id)!.push (n);
        if (s === 0) visit (n);
      }
      state.set (id, 2);
    };
    for (const r of roots) if (! state.has (r.id)) visit (r.id);

    // columns: longest path from the sources
    const col = new Map<string, number> ();
    const indeg = new Map<string, number> ();
    for (const id of ids) indeg.set (id, 0);
    for (const [, ns] of fwd) for (const n of ns) indeg.set (n, indeg.get (n)! + 1);
    const queue = [...ids].filter (id => indeg.get (id) === 0);
    for (const id of queue) col.set (id, 0);
    while (queue.length) {
      const id = queue.shift()!;
      for (const n of fwd.get (id)!) {
        col.set (n, Math.max (col.get (n) ?? 0, col.get (id)! + 1));
        indeg.set (n, indeg.get (n)! - 1);
        if (indeg.get (n) === 0) queue.push (n);
      }
    }
    // sinks pulled no further left than needed: a source that only feeds a
    // late column moves right, next to what it feeds
    const order = [...ids].sort ((a, b) => col.get (b)! - col.get (a)!);
    for (const id of order) {
      const outs = fwd.get (id)!;
      if (outs.length && pred.get (id)!.size === 0)
        col.set (id, Math.max (col.get (id)!, Math.min (...outs.map (n => col.get (n)!)) - 1));
    }

    const nCols = Math.max (...[...col.values()]) + 1;
    const cols: string[][] = Array.from ({ length: nCols }, () => []);
    for (const id of ids) cols[col.get (id)!].push (id);
    for (const c of cols) c.sort ((a, b) => byId.get (a)!.y - byId.get (b)!.y);

    // barycentre sweeps (left → right on predecessors, right → left on successors)
    const rank = () => { const r = new Map<string, number> (); cols.forEach (c => c.forEach ((id, k) => r.set (id, k))); return r; };
    for (let pass = 0; pass < 4; pass++) {
      const forward = pass % 2 === 0;
      const seq = forward ? cols.slice (1) : cols.slice (0, -1).reverse();
      for (const c of seq) {
        const r = rank();
        // neighbour's rank + where the wire sits on the neighbour (0 … <1)
        const bc = (id: string) => {
          const ns = [...(forward ? pred : succ).get (id)!].filter (n => ids.has (n));
          if (! ns.length) return r.get (id)!;
          return ns.reduce ((s, n) => s + r.get (n)! + 0.98 * (forward ? offOn (n, id, 'a') : offOn (id, n, 'b')), 0) / ns.length;
        };
        // fixed items keep their order (they don't move); the others sort around them
        if (c.every (id => byId.get (id)!.fixed)) continue;
        const keyed = c.map ((id, k) => ({ id, k: byId.get (id)!.fixed ? k : bc (id), i: k }));
        keyed.sort ((a, b) => (a.k - b.k) || (a.i - b.i));
        c.splice (0, c.length, ...keyed.map (e => e.id));
      }
    }

    // coordinates: columns side by side, each column centred on the tallest
    // fixed items take no room; a column of fixed items only takes no column
    const real = (c: string[]) => c.filter (id => ! byId.get (id)!.fixed);
    const colW = cols.map (c => Math.max (0, ...real (c).map (id => byId.get (id)!.w)));
    const colH = cols.map (c => { const r = real (c); return r.reduce ((s, id) => s + byId.get (id)!.h, 0) + gapY * Math.max (0, r.length - 1); });
    const height = Math.max (...colH);
    let x = originX;
    cols.forEach ((c, k) => {
      if (real (c).length === 0) return;
      let y = cursorY + (height - colH[k]) / 2;
      for (const id of real (c)) {
        const it = byId.get (id)!;
        const p = { x: snap (x), y: snap (y) };
        out.set (id, p);
        placed.push ({ ...p, w: it.w, h: it.h });
        y += it.h + gapY;
      }
      x += colW[k] + gapX;
    });
    cursorY += height + gapGroup;
  }

  // items with no wires: where they are, or the nearest free spot
  const margin = 16;
  const hits = (r: { x: number; y: number; w: number; h: number }) => placed.some (q =>
    r.x < q.x + q.w + margin && r.x + r.w + margin > q.x && r.y < q.y + q.h + margin && r.y + r.h + margin > q.y);
  const step = opt.grid && opt.grid > 1 ? Math.max (opt.grid, 24) : 24;
  for (const it of isolated) {
    let best = { x: it.x, y: it.y };
    if (hits ({ x: it.x, y: it.y, w: it.w, h: it.h })) {
      search: for (let ring = 1; ring < 200; ring++)
        for (let dx = -ring; dx <= ring; dx++)
          for (const dy of Math.abs (dx) === ring ? range (-ring, ring) : [-ring, ring]) {
            const c = { x: snap (it.x + dx * step), y: snap (it.y + dy * step), w: it.w, h: it.h };
            if (! hits (c)) { best = c; break search; }
          }
    }
    placed.push ({ ...best, w: it.w, h: it.h });
    if (best.x !== it.x || best.y !== it.y) out.set (it.id, best);
  }
  return out;
}

const clamp01 = (v: number) => Math.max (0, Math.min (1, v));
const range = (a: number, b: number) => Array.from ({ length: b - a + 1 }, (_, k) => a + k);
