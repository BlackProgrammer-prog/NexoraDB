# مهاجرت ایندکس v3 — کپی آفلاین و فعال‌سازی دستی

۲۰۲۶-۱۰-۳۰. هیچ DB موجود کاربر در جریان توسعهٔ این تغییر migrate نشده است.

## دلیل و قرارداد

unique نسخهٔ ۲ می‌توانست `1` و `1.0` یا ±zero را جدا قبول کند؛ نسخهٔ ۳ همان equivalence عددی evaluator را اعمال می‌کند.
integerهای بزرگ برای ساخت کلید به double گرد نمی‌شوند. EQ عددی ایندکسی است؛ range عددی هنوز scan صحیح است.
قرارداد کامل: [semantics](TYPE_COMPARISON_SEMANTICS_FA.md).

فرمت: metadata با `NXI3`، کلیدهای `idx3`، reader fence برابر `3` و manifest زیر؛ envelope سند NXD1 باقی می‌ماند:

```json
{"manifest":1,"min_reader":3,"document_keys":1,"document_envelope":1,"indexes":3,"graph_storage":2,"graph_wal":2}
```

reader جدید index قدیمی را درجا rebuild نمی‌کند؛ binary واقعی آرشیوشدهٔ v2 مقصد جدید را رد می‌کند.
نسخهٔ نامعلوم/ناقص، schema نامعتبر، index غیرReady، شمارندهٔ ناسازگار یا دادهٔ orphan باعث توقف migration می‌شود.

## پیش‌نیازها و دیسک

- سرویس‌ها و نویسنده‌های source را متوقف کن؛ ابزار باید LOCK اختصاصی منبع را بگیرد و فقط read-only می‌خواند.
- backup مستقل و مسیر بازگشت را نگه دار. منبع دست‌نخورده می‌ماند؛ checkpoint مستقل اضافی خودکار ساخته نمی‌شود.
- **dry-run یک کپی کامل موقت همراه rebuild و verify است، نه نمونه‌برداری سریع.** فضای یک کپی کامل، WAL و flush/compaction لازم است.
  در WSL هم ظرفیت Linux و هم فضای درایو میزبان VHDX را بررسی کن. حذف فایل در Linux الزاماً اندازهٔ VHDX روی C را فوراً کم نمی‌کند.
- والد مقصد باید موجود باشد؛ خود مقصد تازه و خارج از source باشد. مقصد موجود overwrite نمی‌شود.
- cache/memtable منبع و مقصد هرکدام بودجهٔ ۳۲MiB دارند؛ این سقف RSS کل نیست. metadata ایندکس‌ها در RAM است، نه مجموعهٔ تمام uniqueها.

## ساخت و اجرا

در تنظیم build، `NEXORA_BUILD_RESEARCH_PROBES=ON` لازم است:

```bash
cmake --build build/release --target nexora_index_migrate -j2
```

قالب PowerShell؛ مسیرها نمونه‌اند و باید با DB **بسته** و مقصد تازه جایگزین شوند. این فرمان‌ها روی دادهٔ کاربر اجرا نشده‌اند:

```powershell
wsl -d Ubuntu -- /home/parham/NexoraDB/build/release/nexora_index_migrate --source /path/to/CLOSED_DB --dry-run
wsl -d Ubuntu -- /home/parham/NexoraDB/build/release/nexora_index_migrate --source /path/to/CLOSED_DB --apply --destination /path/to/NEW_DB
```

حالت پیش‌فرض dry-run است و destination نمی‌پذیرد. برای scratch روی حجم مناسب، داخل WSL از پوشهٔ موجود و خصوصی استفاده کن:

```bash
TMPDIR=/path/to/private-scratch build/release/nexora_index_migrate --source /path/to/CLOSED_DB --dry-run
```

API native: `DocEngine.migrate_index_format_copy(source, destination="", dry_run=True)`؛ GIL هنگام کار آزاد است.
CLI در stderr هر ۱۰۰۰ سند progress و در stdout نتیجهٔ JSON چاپ می‌کند.

## روند ایمن و خطا

1. snapshot ثابت زیر LOCK و بررسی checksum منبع؛ هیچ write داده‌ای به source انجام نمی‌شود.
2. کپی schema، metadata داخلی/auth/FK و counter؛ حفظ BSON معتبر و تبدیل JSON قدیمی؛ بازسازی indexها از سند، نه کپی کلیدهای قدیمی.
3. بررسی canonical unique با lookup روی مقصد. collision نام collection/index و دو ID را گزارش می‌کند؛ هیچ سندی حذف یا برنده انتخاب نمی‌شود.
4. مقصد با مجوز Linux برابر 0700 و marker `3-incomplete` ساخته می‌شود. شمارنده‌ها، نبود orphan، checksum و entryهای مورد انتظار/واقعی با سند و مالک تطبیق داده می‌شوند.
5. پس از verify، manifest و marker کامل در یک WriteBatch با sync منتشر می‌شوند؛ قبل از آن reader عادی مقصد را رد می‌کند. موفقیت پس از بستن مقصد گزارش می‌شود.

dry-run در پایان عادی یا exception فقط scratch تازهٔ خودش را پاک می‌کند. SIGKILL ممکن است پوشهٔ خصوصی `nexora-index-audit-*` باقی بگذارد؛ پوشه‌های قدیمی خودکار حذف نمی‌شوند.
شکست apply مقصد ناقص را برای بررسی نگه می‌دارد. resume ندارد؛ پس از رفع علت، مقصد تازه انتخاب کن.

## فعال‌سازی و rollback

نتیجهٔ موفق شامل `verified:true`، `source_unchanged:true` و `activation:"manual; source retained for rollback"` است.
ابزار مسیر سرویس را عوض نمی‌کند. کپی را با reader جدید و queryهای کاربرد بررسی کن، سپس سرویس را آگاهانه منتقل کن.
بازگشت به source **پس از write تازه روی مقصد، آن writeها را حفظ نمی‌کند** و نیازمند reconciliation/backup تازه است؛ source را تا پایان پذیرش حذف نکن.

**فایل‌های graph خارج از document RocksDB کپی یا migrate نمی‌شوند** (`external_graph_files_copied:false`).
برای graph sidecar، backup و cutover سازگار document+graph کار جداست؛ این ابزار migration اتمیک چند store نیست.

## شواهد و محدودیت

`tests/test_index_v3_migration.py` با binary واقعی قبلی در subprocess مستقل: source بدون manifest و نسل ۲، dry-run/apply، برخورد unique، منبع فعال، مقصد nested/existing، SIGKILL وسط کپی، رد مقصد ناقص، retry تازه، خواندن source قدیمی و رد مقصد v3 با reader قدیم.
برای میزبان دیگر `NEXORA_V2_BINARY` را به binary واقعی قبلی بده؛ نبود آن skip صریح است، نه اثبات سازگاری.

این تست‌ها با fixture کوچک Linux/WSL اجرا شدند؛ مقیاس ۱۰۰M، قطع برق، ACL ویندوز و migration آنلاین آزموده نشده‌اند.
نسخهٔ بسته فعلاً 0.4.1 است؛ تغییر فرمت نیازمند artifact/نسخه و راهنمای upgrade هماهنگ پیش از انتشار است.
artifact تازه را با شمارهٔ قبلاً منتشرشده مجدداً منتشر نکن. این ابزار خودش release یا tag نمی‌سازد.
