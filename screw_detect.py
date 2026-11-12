import argparse
import atexit
import os
import shutil
import sys
import tempfile
import time

os.environ.setdefault('PYTORCH_CUDA_ALLOC_CONF', 'max_split_size_mb:128')


def prepare_work_dir(script_dir):
    bases = [tempfile.gettempdir(),
             os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Temp'),
             os.environ.get('TEMP', ''),
             os.environ.get('TMP', ''),
             os.environ.get('USERPROFILE', '')]
    for base in bases:
        if not base:
            continue
        path = os.path.abspath(os.path.join(base, 'screw_work'))
        if path.lower().startswith(script_dir.lower()):
            continue
        try:
            os.makedirs(path, exist_ok=True)
            probe = os.path.join(path, 'probe.tmp')
            with open(probe, 'w', encoding='utf-8') as f:
                f.write('ok')
            os.remove(probe)
            os.chdir(path)
            return path, False
        except Exception:
            continue
    local = os.path.join(script_dir, 'cache')
    try:
        os.makedirs(local, exist_ok=True)
        os.chdir(local)
        return local, True
    except Exception:
        return None, False


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def open_capture(path):
    import cv2
    cap = cv2.VideoCapture(path)
    if cap.isOpened():
        return cap, path
    cap.release()
    if all(ord(ch) < 128 for ch in path):
        return None, path
    try:
        suffix = os.path.splitext(path)[1] or '.mp4'
        tmpdir = tempfile.mkdtemp(prefix='screw_video_')
        tmp = os.path.join(tmpdir, 'input' + suffix)
        shutil.copyfile(path, tmp)
        cap = cv2.VideoCapture(tmp)
        if cap.isOpened():
            log('视频路径包含非 ASCII 字符，已复制到临时文件后读取：%s' % tmp)
            return cap, tmp
        cap.release()
    except Exception as exc:
        log('视频转存失败：%s' % exc)
    return None, path


def main():
    ap = argparse.ArgumentParser(description='YOLOv8 螺丝检测：逐帧统计检测数量并写入 csv')
    ap.add_argument('--model', required=True)
    ap.add_argument('--video', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--conf', type=float, default=0.2)
    ap.add_argument('--imgsz', type=int, default=640)
    ap.add_argument('--stride', type=int, default=1)
    ap.add_argument('--cls', default='auto',
                    help='统计的类别：auto=自动只统计螺丝类，或逗号分隔的类别 id/名称')
    ap.add_argument('--device', default=None)
    ap.add_argument('--max-frames', type=int, default=0)
    args = ap.parse_args()

    if args.stride < 1:
        args.stride = 1

    args.model = os.path.abspath(args.model)
    args.video = os.path.abspath(args.video)
    args.out = os.path.abspath(args.out)

    script_dir = os.path.dirname(os.path.abspath(__file__))
    work_dir, work_is_local = prepare_work_dir(script_dir)
    if work_dir:
        os.environ['TORCHINDUCTOR_CACHE_DIR'] = os.path.join(work_dir, 'torchinductor')
    else:
        log('未能切换到独立工作目录，运行缓存可能写到程序目录')

    if work_is_local and work_dir:
        def _cleanup_local_work():
            try:
                os.chdir(script_dir)
                shutil.rmtree(work_dir, ignore_errors=True)
            except Exception:
                pass
        atexit.register(_cleanup_local_work)

    if not os.path.isfile(args.model):
        log('模型文件不存在：%s' % args.model)
        return 2
    if not os.path.isfile(args.video):
        log('视频文件不存在：%s' % args.video)
        return 2

    log('加载模型：%s' % os.path.basename(args.model))
    from ultralytics import YOLO
    model = YOLO(args.model)

    names = model.names
    if isinstance(names, dict):
        class_names = {int(k): str(v) for k, v in names.items()}
    else:
        class_names = {i: str(v) for i, v in enumerate(names)}
    log('类别：%s' % ', '.join('%d:%s' % (k, v) for k, v in sorted(class_names.items())))

    cls_ids = None
    if args.cls.strip() == 'auto':
        keys = ('screw', 'bolt', '螺丝', '螺栓')
        picked = [k for k, v in class_names.items()
                  if any(word in v.lower() for word in keys)]
        if picked:
            cls_ids = picked
            log('统计类别：%s（其余类别不计入，可用 --cls 覆盖）'
                % ', '.join('%d:%s' % (k, class_names[k]) for k in picked))
        else:
            log('统计类别：全部类别（未找到名称含 screw/bolt 的类别）')
    elif args.cls.strip():
        cls_ids = []
        for token in args.cls.replace('，', ',').split(','):
            token = token.strip()
            if not token:
                continue
            if token.isdigit():
                cls_ids.append(int(token))
            else:
                hit = [k for k, v in class_names.items() if v == token]
                if not hit:
                    log('类别名称不存在：%s' % token)
                    return 2
                cls_ids.append(hit[0])
        log('统计类别：%s' % ', '.join('%d:%s' % (k, class_names[k]) for k in cls_ids))
    else:
        log('统计类别：全部类别')

    cap, real_path = open_capture(args.video)
    if cap is None:
        log('无法打开视频：%s' % args.video)
        return 2

    fps = cap.get(5) or 0.0
    total = int(cap.get(7) or 0)
    width = int(cap.get(3) or 0)
    height = int(cap.get(4) or 0)
    log('视频信息：%dx%d, %.2f fps, 共 %d 帧' % (width, height, fps, total))

    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    fout = open(args.out, 'w', encoding='utf-8', newline='\n')
    fout.write('# video=%s\n' % os.path.basename(args.video))
    fout.write('# fps=%.3f total_frames=%d stride=%d conf=%.2f imgsz=%d\n'
               % (fps, total, args.stride, args.conf, args.imgsz))
    fout.write('frame,count\n')
    fout.flush()

    frame_idx = -1
    processed = 0
    hits = 0
    t0 = time.time()
    stop = False

    try:
        while not stop:
            ok, frame = cap.read()
            if not ok:
                break
            frame_idx += 1
            if frame_idx % args.stride != 0:
                continue

            res = model.predict(frame, conf=args.conf, imgsz=args.imgsz,
                                verbose=False, device=args.device, classes=cls_ids)
            boxes = res[0].boxes
            count = 0 if boxes is None else len(boxes)
            fout.write('%d,%d\n' % (frame_idx, count))
            hits += count
            processed += 1

            if processed % 50 == 0:
                fout.flush()
                elapsed = time.time() - t0
                speed = processed / elapsed if elapsed > 0 else 0
                if total > 0:
                    remain = (total - frame_idx) / (speed * args.stride) if speed > 0 else 0
                    log('\r[推理进度] 帧 %d/%d (%.1f%%)  已处理 %d 帧  用时 %.1fs  预计剩余 %.0fs      '
                        % (frame_idx, total, 100.0 * frame_idx / max(total, 1),
                           processed, elapsed, remain))
                else:
                    log('\r[推理进度] 帧 %d  已处理 %d 帧  用时 %.1fs      ' % (frame_idx, processed, elapsed))

            if args.max_frames > 0 and processed >= args.max_frames:
                stop = True
    finally:
        cap.release()
        fout.close()

    elapsed = time.time() - t0
    log('')
    log('推理完成：处理 %d 帧，累计检出螺丝框 %d 个，用时 %.1fs' % (processed, hits, elapsed))
    log('检测结果已写入：%s' % args.out)

    if real_path != args.video:
        try:
            shutil.rmtree(os.path.dirname(real_path), ignore_errors=True)
        except Exception:
            pass

    if processed == 0:
        log('视频中没有可读取的帧')
        return 3
    return 0


if __name__ == '__main__':
    sys.exit(main())
