# MEM-002 — حافظهٔ زنده، retention و تشخیص allocator

## روش

دو workload جدا با Release+LTO، RocksDB 8.5.3، CPython 3.10.12 و glibc 2.35 روی Linux/WSL اجرا شدند.
هر workload شامل سه جفت process تازهٔ control/trim است. ترتیب جفت‌ها متناوب است؛ هر process بیست چرخه دارد،
چهار چرخهٔ اول warm-up هستند و شانزده چرخه وارد شیب/بازهٔ آماری می‌شوند.
این یعنی ۱۲۰ چرخه برای هر workload، ۲۴۰ چرخه در مجموع و ۴۸ چرخهٔ اندازه‌گیری‌شده در هر گروه هر workload.

دادهٔ هر چرخه: ۱۲٬۰۰۰ سند با payload قطعی ۵۱۲ بایتی، یک ایندکس string، و ring گراف با ۱۲٬۰۰۰ node/edge.
بودجهٔ cache برابر ۳۲MiB و بودجهٔ cache+memtable برابر ۶۴MiB است؛ این اعداد سقف RSS نیستند.

هر چرخه شامل مراحل زیر است:

1. ساخت store خصوصی تازه و load BSON؛ pause شدن migration در این diagnostic صریح است.
2. در workload دوم، flush صریح به SST؛ query ایندکسی range، block cache را گرم می‌کند. workload اول از memtable می‌خواند.
3. بیست query نقطه‌ای ایندکسی و یک query کامل؛ result خام و نسخهٔ decode‌شدهٔ Python هنگام sample زنده هستند و سپس حذف می‌شوند.
4. هشت page cursor، دو snapshot بیرونی گراف و یک snapshot cache؛ اندازهٔ topology با oracle کنترل می‌شود.
5. بستن cursorها و کنترل صفر شدن page live snapshot؛ shutdown گراف و prepare_shutdown سند، با sampleهای جدا.
6. حذف snapshot/manager/engine و GC تشخیصی صریح؛ PMR باید به baseline برگردد.
7. در گروه trim، فراخوانی `malloc_trim(0)` فقط در همین نقطه؛ قبل/بعد و زمان ثبت می‌شوند. control هیچ trim اجرا نمی‌کند.

`tracemalloc` با عمق ۱ فقط در worker تشخیصی و پس از importها روشن است؛ overhead آن جدا ثبت می‌شود.
تمام sampleها **و endpointها** JSONL می‌شوند و تا پایان اندازه‌گیری در RAM انباشته نمی‌شوند.
تحلیل summary بعد از آخرین sample آغاز می‌شود. نسخهٔ اولیه‌ای که endpointها را نگه می‌داشت، کنار گذاشته و artifact آن حفظ شد؛
رشد ناشی از مشاهده‌گر نباید به موتور نسبت داده شود. کمی bookkeeping خود Python/tracer/observer همچنان باقی است.

## شواهد ثبت‌شده

| workload | گزارش خام و تنظیمات | تحلیل |
| --- | --- | --- |
| memtable | `build/mem001002-streamed-release-20261030/report.json` | همان پوشه، `analysis.json` |
| SST و cache گرم | `build/mem001002-sst-release-20261030/report.json` | همان پوشه، `analysis.json` |

هر run، `samples.jsonl`، `endpoints.jsonl`، `summary.json` و stdout/stderr دارد.
hash native، runner، telemetry و تنظیمات allocator/kernel ثبت می‌شوند؛ نسخهٔ sourceهای runner/telemetry نیز کنار report حفظ شده است.
هیچ trim/GC/cache-clear از داخل collector انجام نمی‌شود. flush، GC و trim بالا actionهای نام‌گذاری‌شدهٔ آزمایش هستند.
فقط storeهای تازهٔ ساخته‌شده توسط `TemporaryDirectory` حذف می‌شوند؛ DB کاربر یا نتایج قبلی تغییر نمی‌کنند.

## نتایج memtable

| شاخص پس از destroy، بعد از warm-up | control | trim تشخیصی |
| --- | --- | --- |
| RSS مشاهده‌شده | ۸۷٫۴۶ تا ۹۳٫۸۶MiB | ۳۹٫۴۴ تا ۴۰٫۹۵MiB |
| PSS مشاهده‌شده | ۸۱٫۵۷ تا ۸۷٫۹۸MiB | ۳۳٫۴۸ تا ۳۵٫۰۷MiB |
| شیب RSS در سه process | حدود ۱۵۷، ۲۳ و ۲KiB/چرخه | حدود ۳۷، ۵۹ و ۷۲KiB/چرخه |
| بازهٔ RSS در پنج چرخهٔ آخر هر process | حداکثر ۴KiB تغییر؛ یک process بدون تغییر | حدود ۹۶ تا ۱۳۶KiB تغییر |
| شیب glibc allocated arena+mmap | حدود ۲٫۷ تا ۲٫۹KiB/چرخه | حدود ۲٫۵ تا ۲٫۸KiB/چرخه |
| شیب Python traced current | حدود ۰٫۴۴ تا ۰٫۵۵KiB/چرخه | حدود ۰٫۳۹ تا ۰٫۴۷KiB/چرخه |

درون گروه trim، کاهش RSS قبل/بعد حدود ۴۴٫۲۲ تا ۵۱٫۲۸MiB و میانهٔ آن ۴۸٫۸۳MiB بود.
میانهٔ هزینهٔ trim حدود ۱٫۴۷ms و بیشینهٔ مشاهده‌شده حدود ۱٫۷۸ms بود.
این اندازه‌گیری after-destroy است؛ برابری تقریبی زمان کل چرخه‌ها، برتری سیاست trim در runtime یا p99 سرویس را ثابت نمی‌کند.

نمونهٔ تفکیکی چرخهٔ پنجمِ control:

| مرحله | RSS | subtotal native انتخاب‌شده | PMR snapshot | توضیح |
| --- | --- | --- | --- | --- |
| loaded | حدود ۸۶٫۴۷MiB | حدود ۹٫۰۰MiB | صفر | memtable؛ cache فقط ۸۸ بایت گزارش شده است |
| resultهای query زنده | حدود ۸۷٫۴۵MiB | حدود ۱۵٫۵۵MiB | صفر | Python traced current حدود ۱۷٫۷۴MiB؛ result/query buffer از native subtotal کامل پوشش ندارد |
| snapshotها زنده | حدود ۸۷٫۴۶MiB | حدود ۲۲٫۹۰MiB | حدود ۷٫۳۴MiB | هشت page cursor؛ cache و cloneهای گراف در PMR global فقط یک‌بار |
| cursorها بسته | همان حدود | همان حدود | همان حدود | page count صفر، ولی snapshotهای گراف هنوز owner دارند |
| graph shutdown | همان حدود | همان حدود | همان حدود | manager هنوز زنده است؛ shutdown به‌تنهایی ساختار graph/cache را destroy نمی‌کند |
| document prepare_shutdown | همان حدود | حدود ۲۲٫۹۹MiB | همان حدود | per-DB active/unflushed حدود ۲KiB ولی retained memtables حدود ۸٫۶MiB؛ aliasها جمع نمی‌شوند |
| همهٔ ownerها destroy | همان حدود | صفرِ حوزهٔ انتخاب‌شده | صفر | glibc allocated arena+mmap حدود ۴٫۵۷MiB و free arena حدود ۵۰٫۲۹MiB؛ RSS به صفر تبدیل نمی‌شود |

retained memtableهای پس از prepare_shutdown هنوز به DB زنده و عمر reader/version/TLS داخلی وابسته‌اند؛
این sample به‌تنهایی مالک دقیق همهٔ آن referenceها را اثبات نمی‌کند. تا destroy واقعی، این حافظه در حوزهٔ موتور گزارش می‌شود.
مقایسهٔ «page count صفر» با «کل حافظهٔ موتور صفر» معتبر نیست.

## نتایج SST و block cache گرم

| شاخص پس از destroy، بعد از warm-up | control | trim تشخیصی |
| --- | --- | --- |
| RSS مشاهده‌شده | ۹۳٫۵۷ تا ۱۰۰٫۸۸MiB | ۴۱٫۰۰ تا ۴۱٫۶۴MiB |
| PSS مشاهده‌شده | ۸۷٫۶۳ تا ۹۴٫۹۴MiB | ۳۵٫۰۸ تا ۳۵٫۷۷MiB |
| شیب RSS در سه process | حدود ۴۱۴، ۳۷۹ و ۱۹۹KiB/چرخه | حدود ۲۲، ۳۵ و ۲۴KiB/چرخه |
| بازهٔ RSS در پنج چرخهٔ آخر هر process | هر سه process بدون تغییر | حدود ۵۲ تا ۱۴۸KiB تغییر |
| شیب glibc allocated arena+mmap | حدود ۰٫۹۶ تا ۱٫۶۹KiB/چرخه | حدود ۱٫۰۸ تا ۱٫۳۱KiB/چرخه |
| شیب Python traced current | حدود ۰٫۴۴ تا ۰٫۵۰KiB/چرخه | حدود ۰٫۴۱ تا ۰٫۵۵KiB/چرخه |

شیب کل پنجره با وجود سکوی انتهایی صفر نیست، چون هنوز بخشی از warm-up allocator داخل پنجره باقی می‌ماند؛
بازه و endpointهای پنج چرخهٔ آخر نیز به همین دلیل جدا گزارش شده‌اند. هیچ fit خطی به عنوان رشد نامحدود تعبیر نمی‌شود.

افت RSS قبل/بعد trim در این workload حدود ۵۰٫۵۰ تا ۵۶٫۴۰MiB، با میانهٔ ۵۵٫۲۸MiB بود.
میانهٔ هزینهٔ trim حدود ۲٫۰۰ms و بیشینهٔ مشاهده‌شده حدود ۲٫۶۵ms بود.
بیشینهٔ RSS نمونه‌برداری‌شدهٔ control برابر ۱۰۰٫۸۸MiB و گروه trim حدود ۹۷٫۶۳MiB بود؛ این peak بین stageها را تضمین نمی‌کند.

نمونهٔ چرخهٔ پنجم نشان می‌دهد cache واقعاً گرم شده است: usage از ۸۸ بایت در load به حدود ۹۴٫۵KiB پس از flush
و سپس ۷٫۳۴MiB در query می‌رسد. پس از `prepare_shutdown` usage به **۱۷۸۴ بایت، تماماً pinned** کاهش می‌یابد.
RSS در همان لحظه حدود ۹۳٫۵۷MiB باقی می‌ماند و free arena افزایش می‌یابد؛ آزادشدن cache و برگشت صفحات به OS یک رویداد نیستند.
پس از destroy، PMR صفر و allocated arena+mmap حدود ۴٫۶۵MiB است، اما free arena حدود ۵۷٫۱۶MiB گزارش می‌شود.
این free arena میزان residentِ قابل trim را دقیق مشخص نمی‌کند؛ کاهش واقعی resident در گروه trim جدا ثبت شده است.

در مجموع ۲۲۹۲ sample، اختلاف RSS با چهار جزء private/shared clean/dirty همان rollup **صفر** بود؛
RSS و PSS در تمام این sampleها در دسترس بودند. PMR پس از destroy در هر ۲۴۰ چرخه صفر شد.
hash native و sourceهای آرشیوشدهٔ هر دو workload با report منطبق‌اند.

اعتبارسنجی کد: ۷ تست Python مالکیت/schema، ۲۸ native document و ۲۳ native graph مرتبط،
۲۲ unit lab و سه protocol native کوچک موفق بودند. integration چندسرویس MongoDB/PostgreSQL اجرا نشده است.
smoke دوگروهی CI با native Debug نیز محلی پاس شد. گزارش‌های تست در `build/mem001002-*-20261030.json/xml`
و `build/mem001002-ci-smoke-20261030/report.json` حفظ شده‌اند.

## استنتاج و سطح اطمینان

| یافته | سطح اطمینان و حدود |
| --- | --- |
| PMR snapshot پس از destroy به baseline برمی‌گردد | بالا، برای درخواست‌های ابزارگذاری‌شده؛ wrapper و allocation غیر-PMR پوشش کامل ندارند |
| بخش عمدهٔ RSS باقی‌مانده قابل بازگرداندن توسط allocator است | بالا در همین آزمایش: افت RSS داخل همان process پس از trim ثبت شده و ownerهای موتور قبلاً destroy شده‌اند |
| بخش باقی‌مانده fragmentation است | نامعلوم/احتمال متوسط؛ free arena، keepcost و RSS به‌تنهایی تعداد بایت fragmentation یا صفحات قابل trim را ثابت نمی‌کنند |
| cache/graph پس از shutdown زنده می‌مانند | بالا برای شمارنده‌های همان مرحله؛ registry/cache هنوز owner دارند و با destroy آزاد می‌شوند |
| رشد کوچک allocated/traced به معنای leak موتور است | اثبات نشده؛ bookkeeping مشاهده‌گر/tracer/runtime و allocationهای بدون attribution می‌توانند سهم داشته باشند |
| وجود سقف پایدار برای تمام زمان‌ها و workloadها | اثبات نشده؛ فقط سقف مشاهده‌شده و سکوی کوتاه انتهای آزمایش ثبت شده است |

RSS/PSS شامل residentِ stack، code/library، Python، pageهای آزاد allocator و mappingهای دیگر است.
اختلاف آن با subtotal native، بایت leak یا حافظهٔ دقیقِ بدون مالک نیست؛ [schema و شکاف attribution](MEMORY_OWNERSHIP_FA.md) قواعد این تطبیق را توضیح می‌دهد.

## سیاست runtime

sampler و مرحله‌های protocol دیگر `reclaim_allocator()` را خودکار صدا نمی‌زنند.
مسیر معمول telemetry مشاهده‌گر است؛ trim فقط ابزار opt-in آزمایش تشخیصی بعد از destroy می‌ماند.
عمر cache و shutdown طبیعی حفظ شده است. از این داده‌ها frequency/threshold مناسب runtime، تغییر allocator،
`MALLOC_ARENA_MAX` یا trim در query path انتخاب نمی‌شود؛ latency/refault و workload پایدار سرویس بررسی نشده‌اند.
این تصمیم از افزودن سیاستی که اندازه‌گیری retention را پنهان می‌کند جلوگیری می‌کند.

## بازتولید

```bash
cmake --build build/release --target nexoradb_py --parallel 2
nexora-scale-lab/.venv/bin/python scripts/verify_memory_lifecycle.py \
  --directory build/mem-memtable-REPEAT \
  --native build/release/nexoradb.cpython-310-x86_64-linux-gnu.so \
  --rows 12000 --cycles 20 --warmup 4 --repetitions 3
nexora-scale-lab/.venv/bin/python scripts/verify_memory_lifecycle.py \
  --directory build/mem-sst-REPEAT \
  --native build/release/nexoradb.cpython-310-x86_64-linux-gnu.so \
  --rows 12000 --cycles 20 --warmup 4 --repetitions 3 --flush-before-query
nexora-scale-lab/.venv/bin/python scripts/summarize_memory_lifecycle.py \
  --directory build/mem-sst-REPEAT
```

مسیر report باید تازه باشد؛ overwrite نمی‌شود. سقف ابزار ۵۰k سند، ۲۰ چرخه و چهار تکرار است.
CI یک smoke کوچک از دو گروه اجرا و artifact می‌کند؛ گزارش محلی بالا آزمایش پذیرش محدود بزرگ‌تر از smoke است.
میزبان اختصاصی رزرو نشده، peaks بین stageها نمونه‌برداری نشده‌اند و تخصیص مستقیم mmap/allocator دیگر audit نشده است.
هیچ نتیجهٔ ۱۰M/۱۰۰M، بار بلندمدت سرویس یا تضمین shutdown تحت pressure از این تیک حاصل نمی‌شود؛ MEM-007 جدا باقی می‌ماند.
