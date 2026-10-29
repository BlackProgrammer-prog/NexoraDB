# RES-003 — فرضیه‌های قابل رد، نسخهٔ ۱

ثبت اولیه: ۲۰۲۶-۱۰-۲۸؛ قرارداد preregistration مرحلهٔ صفر. freeze توسط `scripts/research_preregister.py` شامل متن دقیق این سند و ماتریس و SHA baseline اعتبارسنجی‌شده است؛ انتخاب candidate B و admission رقبا قبل از مقایسهٔ واقعی همچنان لازم است.
تمام thresholdها پیشنهادیِ ازپیش‌تعیین‌شده‌اند، نه نتیجهٔ آزمایش. پس از دیدن نتیجه تغییر نکنند؛ تغییر بعدی نسخهٔ تازه می‌خواهد.

| ID | ادعای محدود و baseline | متغیر مستقل | کنترل‌ها | پذیرش / رد |
| --- | --- | --- | --- | --- |
| H1 سرعت | Nexora candidate در W1 نسبت به artifact ثابت A حداقل ۱۵٪ throughput بیشتر دارد | فقط یک patch با دو binary مستقل | dataset/trace/index/durability/build flags/RAM/CPU/transport برابر | median نسبت blockها>=1.15 و کران پایین CI95 bootstrap block>1؛ در غیر این صورت رد/نامعین طبق uncertainty |
| H2 latency | در indexed read تک‌کلاینت، p99 candidate حداقل ۲۰٪ کمتر از A است | patch read path | working set، cache condition، تعداد سند/بایت پاسخ یکسان | نسبت p99<=0.80 و کران بالای CI95<1؛ هیچ correctness error |
| H3 memory | W3 با سقف ۲ GiB بدون throttle و OOM و close<=۱۰ثانیه اجرا می‌شود | budget داخلی cache/memtable | cgroup scope یکسان، swap=0، داده و trace ثابت | همهٔ ۵ repetition و correctness کامل پاس؛ هر high/OOM/timeout نقض فرضیه |
| H4 security | token فاقد collection scope در corpus query/page/stream نمی‌تواند دادهٔ ممنوعه بخواند | role/scope و endpoint | baseline توکن مجاز با همان داده؛ بدون توکن، امضای نامعتبر، collection grant خالی و scope ناکافی در endpointهای query، page و stream | صفر unauthorized success یا leak در body/error/stream؛ هر counterexample رد؛ ادعای امنیت کلی نیست |
| H5 کاربرد | ده workflow مستند از محیط تمیز بدون تغییر دستی کد اجرا می‌شوند | نسخهٔ package/docs | Linux x64/Python3.10؛ install/import، create، insert، indexed lookup، update، transaction rollback، graph build، traverse، algorithm، close/reopen با fixture ثابت | ۱۰/۱۰ خروجی صحیح؛ baseline A همین artifact ثبت‌شدهٔ ۰.۴.۱ و متن freeze‌شده، نه نسخهٔ قدیمی با SHA نامعلوم؛ شمار خطا و گام‌های دستی گزارش؛ جایگزین user study نیست |
| H6 deployment | W2 هزینهٔ end-to-end Nexora نسبت به Neo4j CE کاهش دارد | کل deployment انتخابی، نه فقط هسته | oracle مجموعه/فاصله/field، client TCP، hydration و resource limits | پس از adapter معتبر: >=۱۰٪ median throughput و CI95 پایین>1، latency بدتر از ۱۰٪ نشود؛ N/A تا آن زمان |

## روش تصمیم‌گیری

- واحد مستقل یک اجرای DB تازه است، نه هر request؛ حداقل ۵ block مستقل پس از pilot. آستانهٔ اختلاف endpointهای ABA=۱۰٪ و CV throughput<=۱۰٪؛ drift بالاتر نتیجه را نامعین می‌کند.
- همهٔ نمونه‌ها، خطاها و نتایج منفی نگه داشته شوند. تغییر threshold یا حذف run پس از دیدن نتیجه ممنوع؛ خرابی با علت explicit ثبت شود.
- CI روی نسبت blockهای مستقل با seed و resampling ثبت‌شده؛ `research_ab.py` اکنون CI95 percentile bootstrap با ۵۰۰۰ resampling روی median نسبت blockها می‌دهد؛ کمتر از پنج block exploratory است. قبول فرضیه خودکار نیست و eligibility/correctness/drift باید جدا بررسی شوند.
- p99 از histogram compatible و شمار درخواست کافی به‌دست آید؛ smoke سی‌عملیاتی شاهد H2 نیست. failures/timeouts نباید از denominator حذف شوند.
- H4/H5 معیار عملکرد نیستند و با ops/s جمع نمی‌شوند. benchmarkهای query از مقایسهٔ HA، feature completeness یا امنیت کلی نتیجه نمی‌دهند.
- baseline فایل immutable و SHA ثبت‌شده است؛ اسم نسخه یا نام branch جای artifact مشخص را نمی‌گیرد. هر دو binary فعلی در config نمونه یکی هستند: **A/A کنترل ابزار**، نه اثبات بهبود.
- baseline H1/H2/H3/H5 همان SHA بارگذاری‌شده در ثبت freeze است. baseline H4 رفتار authorized/unauthorized همین artifact است. baseline H6 طرح `Neo4j Community 5.26.32` با Cypher5 در config محصول است؛ binary و effective settings آن تا نصب و admission عمداً نامعلوم می‌ماند و H6 تا آن زمان اجرا نمی‌شود.
- اگر داده invalid/correctness fail است، نتیجه «نامعتبر»؛ اگر drift یا sample count ناکافی است «نامعین»؛ اگر eligible و CI/اثر از آستانه عبور کند «پشتیبانی از فرضیه»؛ اگر eligible ولی کران CI بهبود لازم را رد کند «رد»؛ overlap با آستانه «نامعین». از صرف پاس‌نشدن smoke نتیجهٔ رد علمی گرفته نشود.

وضعیت تمام فرضیه‌ها: **آزموده‌نشده**. smoke فقط مسیر اجرای ابزار و oracle را بررسی می‌کند.
