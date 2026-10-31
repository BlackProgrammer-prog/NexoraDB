# قرارداد reservation مشترک و اجرای محدود — MEM-003/004

تاریخ بررسی: ۲۰۲۶-۱۰-۳۱. این سند قرارداد backend اجرایی را مشخص می‌کند. توسعهٔ grammar و قرارداد کامل SQL در QUERY-001/002/003 همچنان باز است؛ `ORDER BY` و `GROUP BY` هنوز دستور قابل اجرای زبان نیستند.

## بودجه و مالکیت

`ResourceBudget` یک مخزن thread-safe از اعتبار admission است. پیش‌فرض native یک مخزن مشترک process-wide با ظرفیت ۱GiB است؛ `RocksSettings.execution_budget` امکان اتصال چند engine و graph به مخزن صریح مشترک را می‌دهد. مخزن‌های جدا سقف مشترک ایجاد نمی‌کنند. تخصیص‌های متصل ابتدا اعتبار می‌گیرند و با پایان عمر مالک آزاد می‌کنند؛ درخواست پیش‌فرض بدون انتظار رد می‌شود. `reserve(..., wait_ms=...)` انتظار محدود، حداکثر ۶۰ ثانیه، دارد؛ [fairness سرویس و قرارداد shutdown در MEM-005/006](ADMISSION_SHUTDOWN_FA.md) روی این ledger سوارند و پذیرش عمومی executor در CON-005 باز است.

گزارش JSON شامل `pool_id`، `capacity_bytes`، `reserved_bytes`، `peak_reserved_bytes`، `rejected_total`، `waits_total` و `components` است. جمع componentها برابر اعتبار رزروشده است. این اعداد **اعتبار admission هستند، نه اندازه‌گیری تخصیص یا hard RSS**؛ با RSS/PSS یا cache usage جمع نمی‌شوند.

| مؤلفه | مسیرهای متصل و زمان release |
|---|---|
| query | نتیجهٔ document/point query، native join، پاسخ محدود Python و خروجی job؛ تا پایان عمر نتیجه |
| buffer | ورودی write، scratch decode، staging mutation/transaction، شناسه‌های update/delete، صفحهٔ stream و bulk graph؛ تا پایان scope/transaction/builder |
| snapshot | metadata cursor سند و تخصیص واقعی upstream در PMR snapshot گراف؛ تا رهاشدن آخرین مالک |
| graph | برآورد محافظه‌کارانهٔ high-water گراف زنده؛ تا destroy، بدون refund روی حذف/clear |
| workspace | reservation اعلام‌شدهٔ job الگوریتم؛ تا پایان اجرای job |

رشد نتیجهٔ native قبل از تعویض buffer، موقتاً ظرفیت قبلی و جدید را رزرو می‌کند. شکست admission خطای `resource_exhausted` می‌دهد؛ binding مستقیم `ResourceExhausted` از خانوادهٔ `MemoryError` است. رد شدن حافظه نباید engine را وارد وضعیت خطای دیسک کند یا نتیجهٔ ناقص را موفق معرفی کند. `COUNT` تطبیق‌یافته خروجی کامل سندها را جمع نمی‌کند و decode برای تشخیص corruption باقی می‌ماند.

staging تراکنش اعتبار محافظه‌کارانهٔ high-water نگه می‌دارد؛ تلاش ناموفق یا no-op نیز ممکن است تا commit/rollback/reset اعتبار داشته باشد. snapshot سند metadata را حساب می‌کند؛ حجم SST/versionهای pin‌شدهٔ RocksDB هنوز اندازه‌گیری دقیق ندارد. PMR snapshot گراف baseline admission را با تخصیص واقعی دوباره جمع نمی‌کند. workspace الگوریتم برآورد admission است و malloc دلخواه plugin را رهگیری نمی‌کند.

native lookup join فعلی spill ندارد: admission ثابت ۱۲۸MiB، batch یک سند، حداکثر stored document برابر ۱MiB و خروجی تجمعی ۱۶MiB دارد؛ عبور، خطای روشن و نتیجهٔ خالی می‌دهد. این انتخاب مصرف را محدود می‌کند و می‌تواند throughput را کاهش دهد؛ semantics قبلی first-match را به join کامل QUERY-003 تبدیل نمی‌کند.

## sizing سرویس

`size_execution_budget` همهٔ حاشیه‌ها را صریح می‌گیرد:

```text
execution = process envelope - effective Rocks budget
            - allocator margin - Python margin - charged page-cache margin - safety margin
```

`NEXORADB_RESOURCE_PROCESS_MB` سقف برنامه‌ریزی اختیاری است؛ صفر یعنی سقف OS استنتاج نمی‌شود. بعد از auto-tuning مؤثر Rocks محاسبه انجام می‌شود. حاشیه‌های `NEXORADB_RESOURCE_{ALLOCATOR,PYTHON,PAGE_CACHE}_MARGIN_MB` هرکدام پیش‌فرض ۱۲۸MiB و `NEXORADB_RESOURCE_SAFETY_MARGIN_MB` برابر ۶۴MiB هستند. `NEXORADB_EXECUTION_MEMORY_MB` ظرفیت صریح اختیاری است؛ ظرفیت بزرگ‌تر از فضای باقی‌مانده، پیش از بازکردن DB رد می‌شود. سرویس برای ظرفیت یکسان مخزن مشترک نگه می‌دارد. در چند engine باید بودجهٔ واقعی منابع مستقل Rocks را در envelope جمع کرد.

cache/memtable، allocator retention، Python interpreter، نسخه‌های pin‌شده و charged page cache خارج از این ledger هستند. کپی یا انباشت خروجی توسط caller، helper قدیمی خارج از مسیر ابزارگذاری‌شده و تخصیص دلخواه extension نیز تضمین hard limit ندارند. حاشیه‌ها باید با telemetry محیط و RSS/PSS تنظیم شوند؛ برای hard process cap به محدودیت مستقل OS نیاز است. کاهش اعتبار یا budget cache به معنی کاهش فوری RSS نیست. سیاست trim خودکار اضافه نشده است.

## scan، sort و aggregate

در `nexoradb.execution`، `QueryPipeline` یک scan صفحه‌ای، sort خارجی صعودی و aggregate گروهی `COUNT`/`SUM` ارائه می‌کند. مصرف‌کننده باید context را ببندد؛ نگهداری همهٔ rowهای stream در کد caller خارج از quota اپراتور است.

```python
from nexoradb.execution import QueryPipeline, SpillDiskBudget

pipeline = QueryPipeline(engine, memory_bytes=16 * 1024**2,
                         disk_budget=SpillDiskBudget(256 * 1024**2),
                         spill_parent="/private/application-spill")
with pipeline.scan("items") as source:
    with pipeline.sort(source, "score") as rows:
        for row in rows:
            consume(row)

with pipeline.scan("items") as source:
    with pipeline.aggregate(source, "category", sum_field="amount") as groups:
        for group in groups:
            consume(group)
```

quota پیش‌فرض ۱۶MiB، حداقل ۱MiB، fan-in پیش‌فرض ۴ و قابل انتخاب ۲ تا ۱۶ است. حداکثر record پیش‌فرض ۸KiB است و باید با quota/fan-in سازگار باشد. preflight عمق حداکثر ۶۴ و اندازهٔ record را پیش از serialization کامل کنترل می‌کند. ورودی بزرگ/نوع پشتیبانی‌نشده خطا می‌دهد. buffer تقریباً با هزینهٔ محافظه‌کارانهٔ `32 × encoded bytes + 1024` برای هر ردیف حساب می‌شود. scan حداکثر ۱۲۸ سند در صفحه می‌خواهد و quota بایت صفحه را نیز تعیین می‌کند؛ cursor در تمام failureها بسته می‌شود.

sort ترتیب `missing < null < bool < number < Unicode string` دارد؛ مقایسهٔ int64/double بدون تبدیل اجباری int به double است. tie با `_id` و سپس ترتیب ورودی حل می‌شود. array/object و عدد غیرمتناهی کلید مجاز نیستند. group، missing و null را جدا نگه می‌دارد و numeric aliasهای برابر را یک گروه می‌کند. SUM فقط int64 و با overflow check است؛ bool عدد SUM نیست. حافظهٔ هر گروه ثابت است.

runها حین تولید در حداکثر ۳۲ سطح compact می‌شوند؛ تعداد metadata فایل‌ها به تعداد کل ردیف‌ها وابسته نیست. merge چندمرحله‌ای fan-in محدود دارد. frame با CRC32، JSON canonical و حد اندازه اعتبارسنجی می‌شود؛ تعداد کل خروجی نیز با ورودی تطبیق می‌یابد. corruption یا truncation به خطا می‌انجامد؛ skip خاموش وجود ندارد. تضمین atomic برای مصرف stream نیست: caller ممکن است پیش از خطای دیرهنگام، prefix دریافت کرده باشد و باید exception نهایی را شکست کل عملیات بداند.

SELECT معمولی زبان به scan و materialization محدود متصل است؛ پاسخ بزرگ خطای quota می‌دهد و باید از streaming استفاده شود. نتیجهٔ `BudgetedRows` تا `close()` یا پایان عمر خود اعتبار نگه می‌دارد؛ projection همان اعتبار را به اشتراک می‌گذارد. مسیر point/transaction/join نیز پیش از decode پاسخ Python quota بررسی می‌کند. این کار grammar جدید SQL اضافه نمی‌کند.

## دیسک، لغو و recovery

هر عملیات یک directory خصوصی mode `0700` و فایل‌های `0600` با exclusive create و `O_NOFOLLOW` می‌سازد. `spill_parent` باید مسیر اختصاصی و تحت مالکیت application باشد. `SpillDiskBudget` قابل اشتراک میان pipelineها است و اعتبار را با rounding بلوک filesystem به‌علاوهٔ allowance metadata هر فایل، پیش از write می‌گیرد؛ این **سهمیهٔ فیزیکی filesystem نیست** و filesystem می‌تواند مستقل ENOSPC بدهد.

لغو cooperative با callback `cancelled` و خطای `query_cancelled` در safe pointها انجام می‌شود. خروج زودهنگام، quota، ENOSPC، EACCES، EIO و corruption فایل‌ها را می‌بندند و workspace را پاک می‌کنند. short write نیز خطا است. اگر خود cleanup شکست بخورد exception حفظ می‌شود و اعتبار دیسک آزاد نمی‌شود؛ فایل باقی‌مانده نباید مصرف صفر گزارش شود.

پس از SIGKILL، `finally` اجرا نمی‌شود. در startup صریحاً `cleanup_abandoned_spills(parent)` را برای parent اختصاصی اجرا کنید. نام directory از ابتدا PID و start ticks لینوکس دارد؛ reaper فقط directory خصوصی متعلق به UID جاری با process مرده را حذف می‌کند. process زنده، نام ناشناس و symlink حفظ می‌شوند. این recovery لینوکس است؛ اجرای عمومی cancellation/shutdown در MEM-006 همچنان باز است.

## شواهد بازتولید

build Debug، ۱۳۲ آزمون document/recovery، ۸۲ آزمون graph و ۸۲ آزمون Python مرتبط و regression موفق شدند. آزمون‌های جدید رقابت چند engine و graph، wait/release، عمر نتیجه/cursor/transaction، sizing، quota، ترتیب و aggregate، failure و orphan recovery را پوشش می‌دهند. workflow `native-quality` اجرای آن‌ها و نگهداری artifact را دارد.

کمپین `scripts/verify_bounded_execution.py` در دو process تازه ۱۰k و ۱۰۰k ردیف را با quota اپراتور ۱MiB sort کرد. ترتیب و checksum چندمجموعه‌ای SHA256 تطبیق داشت؛ اعتبارهای حافظه/دیسک در پایان صفر و parent خالی بود. Python traced peak به‌ترتیب ۸۰٬۸۹۴ و ۹۶٬۵۷۱ بایت؛ RSS high-water هر دو ۲۶۵٬۲۸۹٬۷۲۸ بایت بود که interpreter/native loading را هم شامل می‌شود. peak اعتبار دیسک حدود ۱٫۶۷ و ۱۷٫۲۵MiB بود. این شاهد رشد محدود همین workload است، نه پذیرش ۱۰M/۱۰۰M، leak-proof عمومی یا سقف RSS.

artifact محلی: `build/mem003004-bounded-final-20261031/report.json` همراه hash binary/source؛ گزارش‌های native در `build/mem003004-native-{doc,graph}-final-20261031.json` و Python در `build/mem003004-python-final-20261031.xml` هستند.

بازتولید در محیط Python دارای وابستگی‌های `tests/performance-requirements.txt`:

```sh
python3 -m pytest -q tests/test_resource_budget.py tests/test_bounded_execution.py
python3 scripts/verify_bounded_execution.py --directory build/mem003004-new-run \
  --native build/debug/nexoradb.cpython-310-x86_64-linux-gnu.so
```
