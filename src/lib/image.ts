/**
 * A picture the user picked or dropped, as an image the page can draw. Read as a data: URL, not a
 * blob: one: the packaged app's Content-Security-Policy allows images from 'self' and data: only, so
 * an object URL fails to load there (while the dev server, without that policy, shows it fine).
 */
export function isImageFile(f: File): boolean {
  return f.type.startsWith('image/') || /\.(png|jpe?g|webp|gif|bmp|avif)$/i.test(f.name);
}

export function readImageFile(f: File): Promise<HTMLImageElement> {
  return new Promise((ok, fail) => {
    const reader = new FileReader();
    reader.onerror = () => fail(new Error('读取这张图片失败，请换一张。'));
    reader.onload = () => {
      const img = new Image();
      img.onload = () => ok(img);
      img.onerror = () => fail(new Error('这张图片打不开（可能是不支持的格式），请换一张 PNG、JPG 或 WebP。'));
      img.src = String(reader.result);
    };
    reader.readAsDataURL(f);
  });
}
