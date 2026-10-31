# برنامهٔ جداگانهٔ پذیرش ۱۰۰M

وضعیت ۲۰۲۶-۱۰-۳۱: **فقط طرح؛ اجرا نشده و مجوز خودکار اجرای ۱۰۰M نیست.** مبنا [قبولی محدود دو تکرار ۱۰M](MEMORY_ACCEPTANCE_10M_FA.md) است. آستانه‌ها پیش از اجرای آینده دوباره preregister و binary/config/source ثابت می‌شوند. این سند نتیجه یا extrapolation سرعت نیست.

## پیش‌شرط‌ها

1. رفع مستقل گیت RES-004 و ثبت evidence پذیرفته‌شده؛ هیچ مقایسهٔ عملکردی از نتایج حافظه نتیجه نشود.
2. انتخاب binary Release نهایی، نگهداری SHA و dependency/build metadata و تطبیق با نصب واقعی؛ در صورت تغییر رفتاری از binary آزمون ۱۰M، bridge validation تازه لازم است.
3. دستور صریح کاربر برای اجرای طولانی در ترمینال خودش؛ عدم اجرای هم‌زمان build، benchmark یا تغییر منابع. source و config طی دو تکرار ثابت بمانند.
4. ثبت RAM/swap، cgroup و فضای آزاد Linux **و درایو میزبان VHDX**. heuristic فعلی حدود ۱۲۴GB برای یک job زنده می‌خواهد؛ این تخمین ۵× JSON است، نه تضمین سقف WAL/compaction. دادهٔ ناقص اجرای شکست‌خورده برای forensic حفظ می‌شود و پیش از تکرار بعدی باید ظرفیت دوباره بررسی شود.

## پروفایل پیشنهادی، نیازمند ثبت مستقل

- دقیقاً ۱۰۰٬۰۰۰٬۰۰۰ سند در هر process تازه؛ دو تکرار مستقل با یک attempt، نه resume یک load ناقص؛ seed ۲۰۲۶ و دادهٔ منطقی همان generator.
- فقط NexoraDB embedded؛ A/B/C و indexed-read × کلاینت‌های ۱/۱۰/۵۰؛ working set کامل، index پیش از load؛ warm-up و مدت measurement ابتدا همان ۱۰/۶۰ ثانیه برای bridge، سپس فقط با preregistration جدید تغییر کنند.
- batch انحصاری BSON تا ۱۰k و دو producer؛ durable WAL ACK بدون غیرفعال‌کردن sync برای بهترشدن نتیجه.
- پروفایل ظرفیت حافظهٔ اولیه همان ۳GiB Rocks، cache برابر ۲GiB، high/max/swap-max برابر ۶/۸/۱GiB؛ حفظ این تنظیم **فرض قبولی ۱۰۰M نیست**. اگر فشار یا failure دیده شود، آن پروفایل رد/نیازمند review ثبت می‌شود. هر sizing جدید یک experiment جدید با شناسهٔ تازه است؛ limits وسط measurement بالا نمی‌روند.
- هیچ `malloc_trim` صریح در runtime اندازه‌گیری؛ startup allocator policy ثبت و ثابت. relief موجودِ cleanup به ۸GiB جداگانه ثبت و حد قبلی حتماً restore شود؛ close بدون relief فقط به‌عنوان بازوی تشخیصی مستقل بررسی شود.

پروفایل‌های موجود `compact-100m.json`، `compact-matched-cache-100m.json` و `ingestion-capacity-100m.json` معادل این طرح نیستند؛ بعضی sampled validation، بودجه یا engineهای متفاوت دارند. از آن‌ها با نام «پذیرش حافظهٔ full 100M» استفاده نشود.

## integrity و telemetry

پس از load، پس از هر workload و پس از restore، count دقیق و scan تمام fieldهای ۱۰۰M با oracle و fingerprint baseline لازم است. تعداد visits در این طرح حدود پنج میلیارد ردیف است و می‌تواند بسیار طولانی‌تر از آزمون ۱۰M باشد؛ ETA با یک bridge عملی برآورد می‌شود، نه صرفاً ضرب throughput خواندن cache گرم در ده.

sampler کل عمر job از connect تا close و خروج process، RSS/PSS/HWM ترکیبیِ engine+generator، glibc allocated/free/mmap، native cache/memtable/admission، page/Rocks snapshot و cgroup current/anon/file/swap/events/PSI را ثبت کند. missing metric، فاصلهٔ واقعی نمونه‌ها و هزینهٔ خود sampler گزارش شوند. exact peak گذرا یا attribution کامل ادعا نشود.

close باید prepare و destroy را جدا ثبت کند؛ snapshot، query و اعتبارهای admission پس از prepare بررسی شوند. heap آزادِ retained و file cache با live object جمع یا به‌عنوان leak قطعی معرفی نشوند. raw، expected-write journal، source snapshot و binary هر attempt immutable باقی بمانند.

## گیت قبول/رد

- هر دو تکرار و ۲۴ سلول، صفر failure و تمام full validationها/restoreهای صحیح؛ `completed` کافی نیست.
- هیچ افزایش max/OOM/OOM-kill یا hard cap پنهان پذیرفته نمی‌شود. high، swap یا full PSI مثبت نیازمند تحلیل مرحله‌به‌مرحله و تصمیم صریح است؛ نتیجه خودکار سبز نشود.
- روند PSS و charged anon+swap در مراحل هم‌معنا بعد از warm-up بررسی شود؛ آستانهٔ review اولیه همان ۲۵۶MiB رشد یا ۱MiB/min در پنجرهٔ پایانی است. اگر پنجره ناکافی باشد، نتیجه نامعین می‌ماند؛ threshold پس از دیدن داده تغییر نکند.
- close عادیِ تحت policy اعلام‌شده حداکثر ۳۰ ثانیه؛ timeout گیت پذیرش است، kill timer نیست. worker در حال flush اجباری کشته نشود. signal/deadline فقط cooperative باشد و outcome نوشتن حفظ شود.
- بعد از prepare، page/Rocks snapshot و query فعال و اعتبارهای execution صفر باشند؛ cleanup و restore limits ثبت شوند. شکست cleanup یا close، قبولی را رد می‌کند.

## اقدام بعدی

پیش از هر اجرا، یک config و launcher اختصاصی `memory-validation-100m` با flag صریح `--allow-large`، بررسی فضای میزبان و قرارداد توقف cooperative ساخته و با fixture کوچک آزمایش شود. مسیر interrupt controller قدیمی lab می‌تواند پس از grace به SIGKILL برسد؛ تا اصلاح و آزمون آن، launcher قدیمی مبنای اجرای پذیرفته‌شدهٔ ۱۰۰M نیست. هیچ دستور اجرای خودکار ۱۰۰M در این نوبت فعال نشده است.

قبولی آینده نیز فقط به binary، منابع و workload ثبت‌شدهٔ همان ۱۰۰M محدود خواهد بود؛ ظرفیت graph/API، scale بیشتر و مقایسهٔ محصولات شاهد جدا می‌خواهند.
