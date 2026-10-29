# بررسی و اصلاح مسیر Durable — موارد ۱ تا ۴

۲۰۲۶-۱۰-۲۹. تغییرات opt-in هستند، جز telemetry ثابت native؛ پیش‌فرض Durable تضعیف نشده است.

## ۱. تفکیک هزینهٔ commit و sync

در `operation_metrics().write_path.stages` سه counter ثابت و بدون قفل سراسری جدید اضافه شد:

- `commit`: زمان و تعداد commit واقعی MutationBuilder، commit صریح و exclusive write؛ savepoint را commit نمی‌شمارد.
- `counter_get_for_update`: زمان دریافت شمارنده با قفل تراکنشی؛ شامل انتظار قفل **و lookup storage** است، نه اندازه‌گیری خالص mutex.
- `insert_ddl_lock_wait`: انتظار قفل shared مسیر درج معمولی/BSON و exclusive مسیر bulk.

هر counter دارای `calls`، `total_ns` و `max_ns` است. زمان‌ها sampled، همپوشان و غیرقابل جمع به‌عنوان breakdown صددرصدی‌اند.
اندازه‌گیری‌ها allocation payload یا شناسهٔ کاربر ندارند؛ overhead زمان‌سنج صفر نیست و هنوز A/B انتشارپذیر برای آن نداریم.
histogramهای موجود `wal_sync_latency` و `db_write_latency`، تعداد `wal_syncs` و p99 ACK آزمون مکمل آن‌ها هستند.

ابزار کوتاه `scripts/benchmark_durable_commit.py` برای ۱/۴/۱۰ writer، batchهای ۱/۸،
یک collection یا collectionهای مستقل در **همان DB**، با DB تازه و oracle کامل ساخته و اجرا شد.
هر cell فقط ۳۲۰ سند و دو تکرار داشت؛ serialization BSON قبل از timing انجام شد.

## ۲. مانع واقعی استفاده از group commit

`MutationBuilder::AdjustCounter` کلید `seq:<collection>` را با `GetForUpdate` می‌گیرد و تا commit نگه می‌دارد.
در نتیجه نویسندگان همان collection پشت شمارنده می‌ایستند؛ shared DDL lock به‌تنهایی آن‌ها را serial نمی‌کند.
این با یافتهٔ کوتاه زیر همخوان است:

| بار | کلاینت | سند در هر درخواست | commit / WAL sync | رکورد/ثانیه در دو تکرار |
| --- | --- | --- | --- | --- |
| یک collection | ۱ | ۱ | ۱ | ۴۶۱ تا ۶۰۷ |
| یک collection | ۱۰ | ۱ | ۱ | ۲۲۵ تا ۵۸۴ |
| collection مستقل | ۱۰ | ۱ | حدود ۵ | ۲۷۷۳ تا ۲۹۱۵ |
| یک collection | ۱۰ | ۸ | ۱ | ۴۴۵۸ تا ۴۶۰۵ |

این‌ها **اعداد تشخیصی کوچک همین host** هستند، نه ظرفیت ingestion، نه نتیجهٔ ۱۰۰M یا ادعای برتری بر رقبا.
تکرار دومِ یک collection/۱۰ writer، p99 بدتری داشت؛ نویز پنهان یا بهترین run انتخاب نشده است.
p99 مربوط به ACK هر درخواست/batch است، نه تقسیم latency batch بر تعداد اسناد.

RocksDB در شرایط مستقل همین حالا commitهای هم‌زمان را گروه‌بندی کرد؛ WAL/transaction manager جدید بازنویسی نشد.
قفل شمارنده حذف نشد: حذف ساده یا انتقال آپدیت count به بعد از commit، atomicity سند/ایندکس/count را می‌شکند.
شاردکردن شمارنده نیازمند قرارداد format/migration، خواندن دقیق count، drop/recovery و آزمون mixed-version است؛
برای این اصلاح محدود، تغییر فرمت ذخیره‌سازی پنهان انجام نشد. این مسیر ساختاری همچنان پیگیری لازم دارد.

## ۳. batch فعلی

مسیر `insert_many_bson_buffer` موجود، یک transaction معتبر شامل سند، ایندکس و شمارنده می‌سازد و پس از commit ACK می‌دهد.
آزمون بالا نشان داد batch هشت‌تایی در این بار تشخیصی هزینهٔ sync را بین اسناد تقسیم کرد؛ برای استفاده از آن
نیازی به خاموش‌کردن WAL نیست. `client.insert_many` شبکه نیز مسیر batch موجود خودش را دارد؛
درخواست HTTP معمولی خودکار با درخواست principal دیگر ادغام نشد.

## ۴. micro-batching محدود و اختیاری

`nexoradb.batching.DurableBatchWriter` برای **درج native در یک collection و یک محدودهٔ اعتماد** اضافه شد:

- یک worker و یک صف bounded؛ سقف تعداد و بایت، دادهٔ **درحال‌commit** را نیز حساب می‌کند.
- هر `submit` یک BSON immutable متعلق به صف می‌سازد و فقط Future می‌دهد؛ enqueue ACK نیست.
- Future فقط پس از موفقیت native batch با effective Durable، WAL روشن و manual flush خاموش موفق می‌شود.
- `max_delay_ms` از ۰ تا ۲ قابل تنظیم است؛ پیش‌فرض آزمایشی ۱ms است، نه نتیجهٔ انتخاب بهینهٔ hardware.
  محدودیت coalescing delay سقف latency کل نیست؛ backlog، lock و storage می‌توانند زمان بیشتری بگیرند.
- تعداد/بایت batch پیش‌فرض ۱۲۸/۱ MiB و ظرفیت صف پیش‌فرض ۲۵۶/۴ MiB است؛ `stats()` صف/درحال‌اجرا و حدود را نشان می‌دهد.
  این سقف payload است، نه سقف RSS کل Python/native؛ concatenate، Future و codec نیز حافظه می‌گیرند.
- `BatchQueueFull` یعنی درخواست پذیرفته و نوشته نشده است؛ backpressure صریح است، صف نامحدود نداریم.
- ورودی نیازمند `_id` صریح است. خطای یک batch کل گروه را رد می‌کند؛ validation failure یک سند می‌تواند Future اسناد معتبر همان گروه را هم fail کند.
  native batch اتمیک است؛ prefix موفق کاذب گزارش نمی‌شود. برای بارهایی که خطای مستقل هر سند لازم دارند از این helper استفاده نکنید.
- خطای native commit برای گروه attempted محافظه‌کارانه `outcome_unknown=true` است؛ درخواست‌های صف که هنوز attempt نشده‌اند
  با `false` رد می‌شوند. writer پس از خطا fail-closed می‌شود؛ retry، split fallback یا پنهان‌کردن IO error ندارد.
- cancellation فقط پیش از شروع native مؤثر است؛ cancel موفق روی درخواست درحال‌commit مجاز نیست.
- `close()` صف را drain و worker را join می‌کند؛ timeout یعنی امکان commit هنوز وجود دارد، نه اینکه عملیات لغو شده است.
  ابتدا writer را ببندید و سپس engine را shutdown کنید. callback Future نباید writer را به‌صورت blocking ببندد.

کاربرد در package Python دارای extra `binary` (نه import مستقیم bare `.so` از root که package را shadow کند):

```python
from nexoradb.batching import DurableBatchWriter

# engine باید Durable و collection از قبل ساخته شده باشد.
with DurableBatchWriter(engine, "items", max_records=64, max_delay_ms=1) as writer:
    for offset in range(0, len(documents), 64):
        pending = [writer.submit(doc) for doc in documents[offset:offset + 64]]
        receipts = [future.result(timeout=10) for future in pending]
        # فقط اینجا می‌توان برای این اسناد موفقیت durable اعلام کرد.
```

این helper مسیر عمومی update/delete/transaction را جایگزین نمی‌کند، isolation تراکنش‌های مستقل را با هم merge نمی‌کند
و روی سرویس HTTP، CLI یا dashboard خودکار فعال نیست. ادغام بین کاربرها بدون طراحی ACL/fairness مجاز نیست.
تأخیر داخل commit تراکنشی اضافه نشده است، چون همان قفل شمارنده را بیشتر نگه می‌داشت.
هنوز sweep عملکردی ۰/۰٫۲/۱/۲ms و soak کافی نداریم؛ این مقدارها tuning knob هستند، نه ادعای اثبات‌شدهٔ بهترین latency.

## شواهد و اجرا

- تشخیص: `build/durable-commit-diagnostic-20261029/report.json`، ۲۴ cell موفق با stdout/stderr مستقل.
- crash: `build/group-commit-crash-20261029/report.json`، **۱۲ SIGKILL/reopen واقعی**؛ شامل دو مورد پس از Future ACK
  با worker جدید زنده، بدون close/flush پیش از kill. تمام سند/ایندکس/count و نبود rollback/staging تأیید شد.
- native **۶۷ تست موفق**، از جمله atomicity، IO/sync failure و شمارش commit در برابر savepoint؛
  گروه Python مرتبط در بازاجرای نهایی **۶۸ موفق**، شامل مجموعهٔ کامل helper **۵/۵ موفق**.
- byte/count capacity، ACK فقط پس از commit، timeout close، شکست batch، صف attempt‌نشده، cancellation و Buffered rejection تست شدند.
- تغییرات rebuild و root/package binary همگام شدند؛ داده‌ها، نتایج تاریخی و نسخهٔ ۰٫۴٫۱ تغییر برچسب نخوردند.

دستور کوتاه بازتولید در PowerShell، با **پوشه‌های تازه**:

```powershell
wsl -d Ubuntu -- /home/parham/NexoraDB/build/release-0.4.1-install-test/bin/python /home/parham/NexoraDB/scripts/benchmark_durable_commit.py --directory /home/parham/NexoraDB/build/durable-commit-REPEAT --repetitions 2
wsl -d Ubuntu -- /home/parham/NexoraDB/build/release-0.4.1-install-test/bin/python /home/parham/NexoraDB/scripts/verify_durability_crash.py --directory /home/parham/NexoraDB/build/group-commit-crash-REPEAT --repetitions 2
```

هر cell تشخیصی timeout ۳۰s دارد؛ آزمون انتشارپذیر RES-004/005 و تست بزرگ همچنان جدا هستند.
هیچ تست چندساعته/۱۰۰M، تغییر OS/storage، restart سرویس، commit یا کاهش تضمین Durable انجام نشد.
WRITE-001/002 به دلیل ممیزی ناقص همهٔ lockها، عمومی‌نبودن صف و benchmark بلندِ ناتمام، خودکار تیک نمی‌خورند.
