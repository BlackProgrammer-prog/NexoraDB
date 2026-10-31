# COR-006 — مدل مرجع مستقل و corpus بازتولیدپذیر

۲۰۲۶-۱۰-۳۰. این مدل عمداً کوچک است؛ از کد evaluator/index codec/parser موتور برای ساخت expected استفاده نمی‌کند.
`scripts/reference_model.py` فقط Python standard library دارد؛ numeric equality با `Fraction` دقیق و bool/string مستقل است.

## subset پذیرفته‌شده

- JSON document با `_id` رشتهٔ صریح، ثابت و کوتاه؛ insert، set روی فیلد `v`، delete و scalar EQ روی همان فیلد.
- valueها: int64، finite float، bool، null، missing، UTF-8 و NUL در string؛ array/object برای storage/update/roundtrip، نه structural query equality.
- bulk اتمیک با duplicate-ID rollback؛ مقایسه با single-writeهای **داخل یک transaction مشترک**، نه ادعای هم‌ارزی atomicity با چند commit مستقل.
- explicit transaction محدود به point insert/set/delete، immediate statement result، commit یا rollback؛ concurrency در این مدل sequential عمداً حذف شده است.
- سه collection: scan بدون index، index معمولی روی `v`، و مسیر single؛ پس از هر operation، result/state/count با مدل مستقل تطبیق دارند.
- queryهای EQ، scan/index را تفاضلی بررسی می‌کنند. codec roundtrip با export صفحه‌های ۷تایی و JSON canonical نوع‌حساس بررسی می‌شود.
  ذخیرهٔ int و float در JSON مقایسهٔ نوع‌حساس دارد؛ numeric query equivalence مستقل از نوع storage است.
- همان corpus روی store کاملاً تازه دوباره replay می‌شود و trace/final state باید عیناً برابر باشد.

این subset جای مدل عمومی NexoraQL، unique/FK، graph، multikey، collation، range/aggregate و تراکنش concurrent را نمی‌گیرد.
unique/FK/race در [history suite](ISOLATION_CONSTRAINTS_FA.md) و corpus COR-004 بررسی می‌شوند؛ ترکیب همهٔ این قابلیت‌ها هنوز اثبات نشده است.

## اجرای کوتاه

از root پروژه داخل WSL، پس از rebuild و sync native:

```bash
nexora-scale-lab/.venv/bin/python scripts/verify_reference_model.py \
  --directory build/cor006-my-validation --seed 505006 --seeds 12 --steps 80
```

مسیر خروجی باید تازه باشد؛ پوشهٔ موجود overwrite نمی‌شود. فقط storeهای موقتِ خود runner ساخته و پس از بستن موتور حذف می‌شوند؛ corpus/report حفظ می‌شوند.
هیچ DB کاربر و نتیجهٔ benchmark قبلی حذف یا migrate نمی‌شود. سقف runner: ۱۰۰ seed و ۱۰۰۰ operation در هر seed؛ پیش‌فرض کوچک است.

خروجی report شامل hash binary، hash مدل/runner، seed و فایل corpus هر case است؛ فایل‌های corpus مستقل از RNG آینده قابل replay هستند:

```bash
nexora-scale-lab/.venv/bin/python scripts/verify_reference_model.py \
  --directory build/cor006-replay-new --replay build/cor006-my-validation/corpus-0000.json
```

## failure و regression

در Mismatch، ابتدا `original.json` حفظ می‌شود؛ minimizer با حذف operationهای بی‌اثر و سپس actionهای داخل transaction، همان failure code را نگه می‌دارد و `minimized.json` می‌سازد.
این deletion-based کوچک‌سازی است؛ ادعای کوتاه‌ترین failure یا کوچک‌سازی همهٔ scalarها ندارد.
خطای setup/exception غیرMismatch در report ثبت و اجرای ناموفق برگردانده می‌شود؛ به‌عنوان موفقیت/skip پنهان نمی‌شود و الزاماً shrink ندارد.

رویهٔ هر failure واقعی:

1. original، minimized و report را حفظ و binary/hash را مقایسه کن.
2. replay تازه باید همان failure را نشان دهد؛ نبود بازتولید، موضوع تحقیق است نه مجوز حذف corpus.
3. fixture حداقلیِ بدون دادهٔ حساس را در `tests/corpus/reference/` اضافه کن؛ `test_reference_regression_corpus` خودکار تمام JSONهای آن را اجرا می‌کند.
4. بعد از fix، fixture باید بدون xfail پاس شود؛ seed اصلی را نیز دوباره اجرا کن.

خود مسیر ثبت failure/minimizer/replay با fault تزریقی در oracle runner آزموده شده است؛ این fault فقط تست است و در موتور تزریق نمی‌شود.
در corpus اولیه failure واقعی مدل/موتور پیدا نشد؛ بنابراین fixture ساختگی به‌عنوان bug واقعی ثبت نشده است.
یک corpus دستی قرارداد (`numeric_bulk_rollback.json`) برای مرز عددی، rollback، bulk و Unicode/NUL ثبت شده است؛ مسیر regression خالی نمی‌تواند بی‌صدا موفق شود.

## پذیرش و محدودیت شواهد

تست‌های unit مستقل برای bool/int، ±zero، ۲^۵۳، missing/null، bulk rollback و minimizer وجود دارد؛ تست native generated روی seedهای ثابت نیز اجرا می‌شود.
گزارش اجرای مستقل در `build/cor006-reference-final-20261030/report.json` است.
این شاهد bounded و reproducible است، نه اثبات ریاضی همهٔ ورودی‌ها، fuzz چندساعته یا تست ۱۰۰M.
