# RES-001 — قرارداد workloadهای پژوهشی، نسخهٔ ۱

تاریخ تعریف: ۲۰۲۶-۱۰-۲۸. وضعیت: مشخصات و smoke مرجع؛ نه نتیجهٔ مقایسهٔ محصولات.
این سند با تغییر semantics نسخهٔ تازه می‌گیرد؛ نتیجهٔ قبلی دوباره برچسب نمی‌خورد.

## قرارداد مشترک

- دادهٔ سندی: `scale_lab.dataset` نسخهٔ ۱، seed=2026، کلید رشتهٔ ۱۲رقمی؛ timestamp رشتهٔ UTC، balance برحسب سنت صحیح، null واقعی و array مطابق مولد فعلی.
- ایندکس: primary `_id` و secondary غیرunique روی `username`؛ ساخته‌شده پیش از load. ترتیب، collation باینری و نوع پارامتر string ثبت شوند.
- نوشتن: ACK پس از WAL/journal محلیِ sync؛ replication خاموش و single instance. atomicity کل batch بین موتورهای مختلف مفروض نیست؛ ingestion جداگانه گزارش شود.
- batch بارگذاری ۱۰۰۰ سند؛ هر عملیات OLTP یک سند. مدل document یکسان؛ schema رابطه‌ایِ بهینه‌شده فقط در track جداگانه.
- load، index build، validation، restore، close و زمان query جدا ثبت شوند. restore خارج از زمان query ولی داخل هزینهٔ کل آزمایش است.
- اجرای هدف: ۵ تکرار مستقل با DB تازه؛ warm-up=۳۰ ثانیه و measurement=۶۰ ثانیه برای هر cell. این اعداد اولیه‌اند و پس از pilot، پیش از مقایسهٔ واقعی freeze می‌شوند.
- تمرکز اولیه closed-loop؛ نرخ عرضه‌شده، موفق/ناموفق، throughput و histogram latency شامل خطا ثبت شوند. p99 برای نمونهٔ کوچک فقط تشخیصی است.
- concurrency هدف ۱ و ۱۰؛ یک cell مستقل برای هر مقدار. smoke فعلی فقط یک کلاینت است.
- transport دو track مجزا: embedded engine capacity و TCP deployment. مقایسهٔ embedded با server فقط «هزینهٔ deployment» است نه برتری هسته.
- RAM: cgroup v2 شامل cache فایل، تمام فرزندان موتور و runtime؛ CPU affinity ثابت ۴ CPU منطقی، swap=0. مولد خارجی بودجهٔ جدا دارد. محدودیت WSL/Windows و میزان فضای دیسک نیز ثبت شوند.
- clean source/binary/config hashes و نسخهٔ driver/موتور قبل از freeze لازم‌اند. خطای correctness نتیجه را invalid می‌کند، نه اینکه صرفاً از throughput کم شود.

## W1 — document OLTP

کاربرد: پروفایل حساب کاربری و تغییر status؛ نه انتقال وجه چندسندی.

| پارامتر | مقدار هدف |
| --- | --- |
| حجم | pilot=۱۰هزار، مبنا=۱میلیون، scale=۱۰میلیون فقط با مجوز |
| read/write | دقیقاً ۵۰/۵۰؛ lookup username و update status بر اساس primary key |
| concurrency | ۱ و ۱۰؛ کلیدهای write هر کلاینت partition مستقل برای oracle قطعی |
| RAM | ۸ GiB کل موتور؛ بودجهٔ cache/memtable داخلی Nexora حداکثر ۳ GiB |
| دادهٔ برگشتی | سند کامل؛ نه صرفاً count یا existence |
| صحت | lookup باید دقیقاً یک سند کامل با همان fieldها برگرداند؛ state نهایی و count با replay trace برابر باشد |

عملیات مفهومی: `SELECT * FROM users WHERE username = :username LIMIT 2` و
`UPDATE users SET status = :status WHERE _id = :id`؛ syntax پارامتر به adapter وابسته است.
ذخیرهٔ trace نهایی و ترتیب ACK برای نسخهٔ concurrent لازم است. trace تصادفی از seed و شمارهٔ عملیات ساخته شود، نه ساعت.

## W2 — query ترکیبی document + graph

کاربرد: پیدا کردن ارتباط دوگام و سپس بازیابی پروفایل مقصد.

- رأس‌ها همان اسناد W1؛ برای رأس i سه edge جهت‌دار به `(i+1)%N`، `(i+7)%N` و `(i+13)%N`، N>=32. شناسهٔ edge یکتا و بدون parallel edge است.
- pilot=۱۰۰۰ رأس/۳۰۰۰ edge؛ مبنا=۱۰۰هزار رأس/۳۰۰هزار edge. concurrency=۱ و ۱۰، RAM=۸ GiB، read/write=۱۰۰/۰ پس از build ثابت.
- query: تمام رأس‌های قابل‌دسترسی در فاصلهٔ ۱ یا ۲، بدون مبدأ، با کوتاه‌ترین distance؛ سپس hydration سند کامل هر مقصد. ترتیب پاسخ مهم نیست؛ مجموعهٔ ID و distance و تک‌تک fieldهای سند مهم‌اند.
- یک operation برابر traversal به‌اضافهٔ hydration همهٔ نتایج است؛ شمارش آن به‌عنوان چندین query ممنوع. حداکثر reachable این fixture از ۱۲ کمتر یا مساوی است؛ LIMIT=1000 نباید truncate کند.
- oracle مستقل: BFS با set روی فرمول edgeها، نه خروجی خود موتور. duplicate/path multiplicity و ترتیب traversal با semantics مرجع یکی نیستند و normalize می‌شوند.
- NexoraQL: `TRAVERSE User('000000000000') OUT LINK DEPTH 2 LIMIT 1000;` سپس native document lookup. composite snapshot و همگامی هم‌زمان سند/گراف ادعا نمی‌شود.
- زمان build، startup/recovery و RAM گراف جدا گزارش شوند. `PROPERTIES`/filter ناقص mapping در این سناریو استفاده نشود.
- Mongo `$graphLookup`، PostgreSQL recursive CTE، Cypher و SQL Graph فقط پس از پیاده‌سازی و آزمون oracle وارد مقایسه شوند. نبود adapter به معنای نبود قابلیت محصول نیست.

## W3 — محدودیت حافظه

- همان اسناد و username lookup؛ read/write=۱۰۰/۰؛ pilot=۱۰هزار، مبنا=۱میلیون؛ concurrency=۱ و ۱۰.
- بودجهٔ سخت کل deployment=۲ GiB، soft high=۱٫۵ GiB، swap=0؛ sweep جداگانهٔ ۱/۲/۴ GiB تنها پس از pilot. موتور فاقد امکان اجرای معتبر در این بودجه با «unsupported resource profile» ثبت شود، نه صفر ops/s.
- Nexora cache/memtable داخلی حداکثر ۵۱۲ MiB؛ buffer محدود، cursor بسته‌شده، snapshot آزادشده. RSS به‌تنهایی معیار cgroup نیست.
- خروجی: throughput، p95/p99، memory.current/peak، anon/file، PSI، high/max/OOM/oom_kill، زمان close و رشد WAL/SST. مرجع صحت همان W1 بدون write است.
- معیار اولیهٔ قبول حافظه: هیچ OOM یا high-event جدید، close<=۱۰ثانیه و state کامل صحیح. نقض limit، حذف نمونه یا افزایش پنهانی limit ممنوع است.

## اجرای کوچک موجود و فاصله تا پذیرش

`scripts/research_workload.py` روی native واقعی W1/W2/W3 را با oracle کامل در DB موقت می‌آزماید؛ سقف ۱۰هزار رکورد، ۱ یا ۱۰ کلاینت، warm-up ده عملیات برای هر کلاینت. نسخهٔ trace اکنون ۲ است و کلیدهای هر worker partition مستقل دارند؛ خروجی‌های trace قدیمی به نسخهٔ تازه برچسب نمی‌خورند.
این driver oracle را در حافظه نگه می‌دارد و در مسیر timed هم صحت را بررسی می‌کند؛ بنابراین **برای ظرفیت یا اندازه‌گیری خالص هسته نیست**.
اجرای standalone فقط budget داخلی ۳۲ MiB را برای W3 تنظیم می‌کند و `memory_limit_enforced=false` می‌دهد. wrapper `scripts/research_cgroup.py` سقف سخت ۲ GiB، high=۱٫۵ GiB و swap=0 را به process جدید و فرزندانش اعمال و عضویت و counterها را بررسی می‌کند؛ controller و سرویس‌های فعلی جابه‌جا نمی‌شوند.
این runner قراردادهای سه سناریو را با حجم کوچک قابل اجرا می‌کند؛ ادعای ظرفیت در حجم مبنا/scale، فشار واقعی نزدیک سقف RAM و adapterهای peer همچنان نیازمند مراحل ارزیابی بعدی‌اند.

خارج از محدودهٔ نسخهٔ ۱: HA/replication، sharding، تراکنش چندسندی، analytics عمومی، vector search، guarantee امنیت عمومی و ۱۰۰M.
