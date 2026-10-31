# پذیرش حافظهٔ ۱۰M — MEM-007

تاریخ تصمیم: ۲۰۲۶-۱۰-۳۱. نتیجهٔ ممیزی: **قبول محدود در ۱۰ میلیون سند** (`accepted_10m_limited`)؛ بدون failure یا review حل‌نشده در گیت‌های همین پروفایل. این تصمیم، پذیرش ۱۰۰M، graph/API، اثبات عمومی نبود leak یا تأیید عملکرد مقایسه‌ای نیست. وابستگی مستقل RES-004 هنوز باز است و مانع اعلام آماده‌بودن انتشار 0.5.0 می‌ماند.

## شواهد و دامنه

اجرای کاربر: `nexora-scale-lab/results/mem007_user_20261031`، از ۰۷:۲۱:۳۳ تا ۱۰:۴۸:۱۶ UTC، برابر ۱۰:۵۱:۳۳ تا ۱۴:۱۸:۱۶ تهران؛ حدود ۳ ساعت و ۲۷ دقیقه. اجرای قبلی `mem007_20261031_current` به درخواست کاربر متوقف شد و در پذیرش وارد نشده است. هیچ تاریخچه‌ای بازنویسی یا attempt شکست‌خورده‌ای پنهان نشده است.

- دو process مستقل، هرکدام fresh load برابر ۱۰M؛ یک attempt برای هر تکرار؛ نه یک store با ۲۰M سند.
- workloadهای YCSB-A/B/C و indexed-read، با ۱/۱۰/۵۰ کلاینت، مجموعاً ۲۴ سلول؛ warm-up ده ثانیه و measurement شصت ثانیه برای هر سلول.
- seed برابر ۲۰۲۶، batch برابر ۱۰k، username index پیش از load، مسیر embedded Python/native و ingestion انحصاری BSON با دو producer.
- بودجهٔ Rocks برابر ۳GiB شامل cache با ظرفیت ۲GiB؛ admission pool اجرای native برابر ۱GiB که تخصیص واقعی یا RSS نیست.
- cgroup با `memory.high=6GiB`، `memory.max=8GiB` و `memory.swap.max=1GiB`؛ processهای مولد داخل همین محدودیت‌اند.
- Release+LTO، بدون sanitizer، GCC 11.4 و Python 3.10.12 در WSL/Linux. متادیتای binary آزمایش‌شده هنوز 0.4.1 است و شامل working-tree تغییرات این دوره است؛ این، binary تاریخیِ منتشرشدهٔ 0.4.1 نیست.

native SHA256 مشترک هر دو تکرار:
`ed8e1cb2ed833a49f0d2bd505b601257cf133a14c6882fd3ed1636d7cf0cda71`

source snapshot SHA256:
`16526c5a9c7af329a1bee6c09df970874a1a36c78ebd36acb84eca3f70089638`

config SHA256:
`260101ad43c27f4e57c133a4cf555a592c8677740d53a9d32477e8482726e2c0`

source snapshot و کپی binary واقعی برای هر تکرار حفظ و با hash تطبیق داده شدند. نتایج این اجرا به نسخهٔ آینده با hash متفاوت منتقل نمی‌شوند مگر delta و validation آن نسخه صریح ثبت شود.

## integrity و outcome

پس از هر load، تمام fieldهای ۱۰M سند بررسی شدند. پس از هر workload نیز scan کاملِ تطبیق با oracle writeها و پس از restore، scan کامل تطبیق با baseline انجام شد: مجموعاً ۵۰ validation کامل روی storeهای ۱۰M. تعداد بازدید سند، ظرفیت تأییدشده را به ۱۰۰M یا ۵۰۰M تبدیل نمی‌کند.

baseline چندمجموعه‌ای SHA256-sum/xor هر دو تکرار یکسان بود و تمام ۲۴ سلول `verified-and-restored` شدند. تکرارها به‌ترتیب ۱۰٬۷۵۵٬۸۶۰ و ۱۰٬۱۱۸٬۸۸۷ عملیات اندازه‌گیری‌شده، با **صفر failure** داشتند. هیچ sampled validation به‌جای full validation استفاده نشده است. ACK مؤثر document از نوع durable، با WAL فعال و sync-on-ACK بود؛ این آزمایش power-loss یا atomic document+graph نیست.

## حافظه و فشار

| شاخص | تکرار اول | تکرار دوم |
|---|---:|---:|
| peak نمونه‌برداری‌شدهٔ RSS ترکیبی | ۳٫۲۴GiB | ۳٫۱۴GiB |
| peak نمونه‌برداری‌شدهٔ PSS ترکیبی | ۳٫۲۳GiB | ۳٫۱۳GiB |
| peak نمونه‌برداری‌شدهٔ charged cgroup | ۵٫۶۱GiB | ۵٫۶۲GiB |
| peak swap | صفر | صفر |
| delta رخدادهای high/max/OOM/OOM-kill | صفر | صفر |
| delta زمان full memory PSI | صفر | صفر |
| peak مشاهده‌شدهٔ page snapshot | ۱ | ۱ |
| page snapshot / Rocks snapshot / active query پس از prepare | ۰ / ۰ / ۰ | ۰ / ۰ / ۰ |
| اعتبار execution باقی‌مانده پس از prepare | صفر | صفر |
| prepare shutdown | ۱٫۰۳۹s | ۱٫۵۳۷s |
| destroy | ۰٫۰۲۲s | ۰٫۰۱۴s |
| close کل | **۱٫۰۶۲s** | **۱٫۵۵۱s** |

هیچ missing metric در sampler کل job ثبت نشد. تعداد sampleها ۵۶۵۰ و ۵۶۵۱ بود؛ زمان هر job حدود ۶۱۴۴ و ۶۲۵۹ ثانیه است. sampling دوره‌ای است و exact peak همهٔ تخصیص‌های کوتاه‌عمر را اثبات نمی‌کند. هزینهٔ CPU sampler کل job حدود ۲۶۳ و ۳۸۲ ثانیه، تقریباً ۴٫۳٪ و ۶٫۱٪ یک core، داخل اجرای مشاهده‌شده است؛ این اعداد throughput بدون instrumentation نیستند.

در ۱۳۸۶ sample مربوط به load و measurement، high/max/swap-max به‌ترتیب همان ۶/۸/۱GiB بودند. بنابراین soft relief وارد measurement نشده است. در telemetry load، `stall_micros=0` و `write_stopped=false` مشاهده شد؛ این ادعای شمارش مستقل stallهای Rocks در تمام مراحل نیست. عدم throttling حافظه از delta صفر high و PSI همین cgroup نتیجه می‌شود، نه صرفاً پایان‌یافتن برنامه.

حداقل فاصلهٔ **مشاهده‌شده** از high فقط حدود ۴۰۱ و ۳۹۲MiB بود. این حاشیه برای تعمیم به دادهٔ بزرگ‌تر، سند بزرگ‌تر یا workload دیگر کافی فرض نمی‌شود. RSS/PSS و charged با هم جمع نمی‌شوند؛ charged شامل page cache و هزینه‌های kernel است و attribution داخلی نیز هم‌پوشانی و gap دارد.

## رشد زنده و allocator retention

گیت از قبل ثبت‌شده، شش مرز پایانی تکمیل cursor را با Theil–Sen بررسی کرد؛ آستانهٔ review برابر رشد بیش از ۲۵۶MiB یا شیب بیش از ۱MiB/min بود. هر دو تکرار `bounded_observed` شدند. شیب PSS به‌ترتیب حدود منفی ۲٫۲۰ و منفی ۰٫۳۷MiB/min و شیب charged anon+swap تقریباً منفی ۰٫۰۰۶ و مثبت ۰٫۰۱۰MiB/min بود. این مرزها پایان cursor هستند و هم‌ارز snapshot اتمیکِ allocationهای زنده نیستند.

پس از destroy، `allocated_arena_bytes` گزارش‌شدهٔ glibc در هر دو process به حدود ۴٫۵MiB رسید، ولی حدود ۳٫۰۰ و ۲٫۸۹GiB arena آزاد همچنان نگه داشته شده بود. charged بلافاصله پس از close نیز حدود ۵٫۴GiB باقی ماند و شامل anon نگه‌داشته‌شده و file cache بود. **این افت‌نکردن فوریِ حافظه به معنی باقی‌ماندن objectهای DB یا leak نیست.** شواهد به نفع allocator retention و charged cache است؛ اندازهٔ دقیق fragmentation و امکان reclaim همهٔ free arenaها اندازه‌گیری نشده است.

`malloc_trim` صریح در sampler/مراحل اجرا فراخوانی نشده است. تنظیم‌های startup موجود `MALLOC_ARENA_MAX=2` و `MALLOC_TRIM_THRESHOLD_=131072` ثبت شده‌اند؛ رفتار خودکار allocator با «هیچ reclaimی رخ نداده» اشتباه گرفته نمی‌شود. سیاست runtime سرویس بر اساس این آزمایش به trim خودکار تغییر نکرده است.

## cleanup و حدود تصمیم

پس از تمام workloadها، validationها و restoreها، policy موجود فقط هنگام cleanup، high را موقتاً از ۶ به ۸GiB رساند و به ۶ بازگرداند؛ hard RAM و swap cap افزایش نیافتند. `cleanup-memory.json` این مداخله و restoration را ثبت کرده است. فیلد `before_close` در آن فایل **پس از ورود به scopeِ relief** ثبت شده و high=8GiB دارد؛ نمونه‌های مستقل job/measurement شاهد حد ۶GiB قبل از این scope هستند. نیاز علّی به relief اثبات نشده، چون close کنترلیِ بدون relief اجرا نشده است. زمان close فقط تحت همین policy تأیید می‌شود.

هر دو store خصوصی فقط پس از success بسته و با ownership check حذف شدند؛ source، binary، checksum، oracle و telemetry حفظ شده‌اند. اجرای SIGTERM/deadline مربوط به MEM-006 شاهد جداگانه دارد؛ این اجرای MEM-007 close عادی را بررسی می‌کند.

خروجی ممیزی: `build/mem007-user-audit-20261031.json`، جزئیات مستقل measurement/allocator: `build/mem007-user-detail-20261031.json`؛ policy از پیش ثبت‌شده: `build/mem007-policy-20261031.json`. گزارش HTML و rawها زیر پوشهٔ اصلی اجرا هستند. هیچ gate برای سبزشدن این نتیجه تغییر نکرده است.

**تصمیم:** پذیرش فنی این پروفایل در اندازهٔ ۱۰M تأیید شد. نبود رشد نامحدود در همهٔ آینده، hard RSS، workloadهای دیگر یا ظرفیت ۱۰۰M اثبات نشده است. [طرح جداگانهٔ ۱۰۰M](MEMORY_100M_PLAN_FA.md) آماده است و اجرا نشده؛ پذیرش عمومی والد و آماده‌بودن انتشار به RES-004 و validation بستهٔ نهایی وابسته می‌ماند.

پیگیری تصمیم کاربر در همان تاریخ: ۱۰۰M فعلاً کنار گذاشته شد و [candidate آزمایشی 0.5.0](../releases/v0.5.0.md) با محدودهٔ روشن ۱۰M و validation بستهٔ نهایی آماده شد. این محدودکردن دامنهٔ انتشار، نتیجهٔ عددی ممیزی یا وضعیت باز RES-004/005 و پذیرش ۱۰۰M را تغییر نمی‌دهد. binary جدید 0.5.0 آزمون‌های bridge و نصب تمیز را گذرانده؛ binary و شواهد ۱۰Mِ اندازه‌گیری‌شده حفظ شده‌اند. snapshot آن پروفایل، C++ و harness آزمایش را ثبت کرده بود و فایل‌های SDK پایتون را شامل نمی‌شد؛ validation جداگانهٔ SDK/بسته و این شکاف provenance در release manifest صریح است.
