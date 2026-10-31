# RES-005 — تفکیک لایه‌ها و حسابداری منابع

وضعیت: قرارداد، پیاده‌سازی و آزمون فنی کوتاه سه‌لایه تکمیل شد؛ پذیرش پژوهشی والد RES-005 به RES-004 و تکرارپذیری عددی وابسته و هنوز باز است. این گزارش ظرفیت نهایی یا مقایسهٔ محصولات نیست.

ابزار جدید: `scripts/research_layers.py` و target `nexora_research_read_probe`؛
یک fixture و trace مشترک به سه process مستقل داده می‌شود، سند کامل با object/array/null بررسی می‌شود و مصرف C++/Python به‌صورت combined و server/client شبکه جدا گزارش می‌شود.

## یافته‌های کد موجود

| ابزار | آنچه واقعاً اندازه می‌گیرد | محدودیت |
| --- | --- | --- |
| `benchmarks/cpp/nexora_benchmark.cpp` | مسیرهای native و workloadهای فعلی C++ | قرارداد داده/نتیجه با HTTP هنوز مشترک نیست |
| `scripts/benchmark_data_path.py` | native JSON، Python decode و ASGI/TestClient | auth override دارد؛ TCP/TLS واقعی نیست؛ tracemalloc شامل native نیست |
| `scripts/benchmark_http.py` | درخواست واقعی curl با کنترل نسخهٔ HTTP | process/connection تازه در هر request؛ latency شامل handshake، oracle معنایی body ندارد |
| `scale_lab.resources` و `adapters.metadata` | process tree موتور، resource roots و host | embedded باید با generator یک scope combined بماند |
| `scripts/research_workload.py` | native binding با oracle در همان process | correctness probe؛ زمان oracle هم داخل operation و RAM آن داخل RSS است |

## قرارداد آزمایش matched پیشنهادی

عملیات پایه: lookup ID روی fixture مشترک ۳۲ تا ۱۰هزار سند؛ trace از ۱۰ تا ۱۰۰هزار ID قطعی؛ فایل immutable مشترک با SHA؛
پاسخ سند کامل، equality تمام fieldها برای هر پاسخ. اجرای پذیرش این مرحله ۱۰۰۰ سند و ۲۰۰ عملیات برای هر لایه بود، نه سقف ظرفیت.

1. **L0 C++:** `DocEngine::FindMany` با شرط EQ روی `_id`، limit=2 روی DB مستقل با همان fixture و تنظیمات durable؛ نتیجهٔ کامل parse و با مرجع مقایسه می‌شود و تعداد بایت پاسخ گزارش می‌شود.
2. **L1 binding:** همان FindMany و شرط و دریافت JSON از pybind؛ `json.loads` و oracle بیرون زمان هر call است ولی داخل پنجرهٔ throughput قرار دارد. parser و HTTP در این track نیستند.
3. **L2 network:** یک server مستقل روی loopback TCP، اتصال persistent client و auth فعال، همان lookup؛ درخواست HTTP و plan واقعی ثبت شود. query parser اگر لازم است در scope بماند؛ native path معادل نامیده نشود.
4. **L2b NexoraQL ablation آینده:** parser cache روشن/خاموش در cell جدا؛ syntax-only cache با result cache اشتباه نشود. این ablation برای پذیرش سه‌لایهٔ پایه لازم نیست و هنوز اجرا نشده است.

در HTTP query از `$id` برای پارامتر AST استفاده می‌شود و oracle روی `raw.statements[].documents` است؛ `rows` برای جدول داشبورد nestedها را به رشته تبدیل می‌کند. هزینهٔ ساخت **هر دو** raw و table projection در این profile باقی می‌ماند؛ تفاضل زمان با C++ «هزینهٔ TCP خالص» نیست. TLS در این آزمون نیست.

warm-up، request count، concurrency و پاسخ باید برابر باشند. cold/warm page cache و block cache جدا برچسب بخورند؛ بدون مجوز cache سیستم drop نشود.
هر اجرا تازگی namespace، count و fingerprint کامل داده را قبل و بعد تأیید کند.

## منابع؛ چه چیزهایی جمع نشوند

- L0: موتور و native generator یک process هستند؛ RSS/CPU آن **combined** است.
- L1: موتور، interpreter، binding و مولد یک process هستند؛ همان مصرف را دوباره به عنوان «engine + Python» جمع نکنید.
- L2: server tree و client tree جدا، بدون PID مشترک؛ RSS جمع‌شده shared pageها را دوباره می‌شمارد، بنابراین PSS و scope لازم‌اند. cgroup process tree شامل file cache است، RSS به آن اضافه نشود.
- allocator retention، cache، memtable و snapshot از counters موتور تفکیک شوند؛ تفاضل RSS و cache اندازهٔ دقیق Python نیست.
- latency end-to-end را می‌توان کنار latency layer probe گزارش کرد؛ تفریق median/p99 دو آزمایش مستقل **زمان دقیق serialization/GIL/network را نمی‌دهد**. برای سهم دقیق span instrumentation همان request و clock مشترک لازم است.
- CPU seconds، wall time، bytes sent/received، allocation count و cache-hitها در کنار ops/s ثبت شوند؛ tracing overhead با on/off مستقل سنجیده شود.

## شواهد پذیرش و ادامه

- [x] fixture/trace مشترک و oracle کامل با hash برای C++/Python/HTTP.
- [x] target C++ مستقل و binding driver با metrics یکسان برای گزارش لایه‌ها؛ ادعای ABI یا مسیر اجرای یکسان نیست.
- [x] network driver با server lifecycle اختصاصی، managed auth واقعی، ACL و کنترل body/statement errors.
- [x] sampler دو process tree با PID start-time و PSS و جلوگیری از PID reuse/double count؛ cgroup فقط در wrapper کم‌حافظه.
- [x] آزمون سند غلط/پاسخ خالی، HTTP200 با statement failure، 401/403 واقعی و cleanup با timeout.
- [x] run کوتاه هر سه لایه و A/A پنج-block جدا برای سنجش noise.

گزارش عددی در `layers/report.json` اجرای پذیرش ثبت شده است. p99 این نمونهٔ کوچک ابزار تشخیصی است؛ برای inference یا ادعای ظرفیت از آن استفاده نشود. instrumentation دقیق serialization/GIL/mutex، TLS و ablationهای بیشتر کار آینده‌اند؛ عدد آن‌ها از تفریق medianها ساخته نشده است.
