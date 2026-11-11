// Place a region of a 3000x2020 dashboard screenshot into a box of width W.
// region = [x, y, w, h] in screenshot pixels.
function crop(el, src, region, W) {
  const [x, y, w, h] = region, k = W / w;
  el.style.width = W + 'px';
  el.style.height = Math.round(h * k) + 'px';
  const img = document.createElement('img');
  img.src = src;
  img.style.width = Math.round(3000 * k) + 'px';
  img.style.left = (-x * k) + 'px';
  img.style.top = (-y * k) + 'px';
  el.appendChild(img);
}
const NS = 'http://www.w3.org/2000/svg';
function svg(parent, tag, attrs, text) {
  const e = document.createElementNS(NS, tag);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  if (text !== undefined) e.textContent = text;
  parent.appendChild(e);
  return e;
}
