# تنظیم وابسته به سخت‌افزار — سیاست v1

۲۰۲۶-۱۰-۲۸؛ سیاست محافظه‌کارانهٔ زمان راه‌اندازی، نه ادعای بهینه‌ترین تنظیم یا برتری بر MongoDB/Neo4j/PostgreSQL.

## تغییر عملی

- در native C++، `cpu_threads=0` تعداد CPUهای affinity جاری Linux را می‌بیند؛ تعداد کل CPU میزبان را
  به‌اشتباه ظرفیت پردازش محدودشده فرض نمی‌کند. مقدار صریح native همچنان مقدم است.
- سرویس admin/API به‌صورت پیش‌فرض `NEXORADB_ROCKS_AUTO_TUNE=true` دارد. قبل از ساخت موتور،
  affinity، `cpu.max` و `memory.high/max/current` تمام والدهای قابل مشاهدهٔ cgroup v2 و حافظهٔ VM/host خوانده می‌شوند.
  برای سهمیهٔ fractional، بودجهٔ worker به سمت پایین گرد می‌شود، با حداقل یک worker؛ pin یا تغییر quota انجام نمی‌شود.
- بودجهٔ cache+memtable برابر حداقل این سه مقدار است: بودجهٔ تعیین‌شدهٔ کاربر، یک‌چهارم سقف حافظهٔ مشاهده‌شده،
  و نصف headroom مشاهده‌شده. مقدار ناشناخته از محاسبه کنار گذاشته می‌شود؛ خروجی به MiB پایین گرد می‌شود.
  بودجه هرگز خودکار بیشتر نمی‌شود. headroom فعلی یک مشاهدهٔ زمان startup است، نه رزرو حافظه برای آینده.
- cache صریح بی‌صدا بازنویسی نمی‌شود؛ اگر با بودجهٔ امن تعارض داشته باشد startup با توضیح خطا می‌دهد.
  cache خودکار نصف بودجه است؛ کمتر از ۴ MiB بودجهٔ موتور قابل قبول نیست.
- نوع دیسک از block device مسیر واقعی DB و `queue/rotational` خوانده می‌شود؛ SSD از نام مسیر یا پسوند NVMe حدس زده نمی‌شود.
  این پرچم در VM/WSL توصیف دستگاه مهمان است، نه اثبات نوع دیسک فیزیکی Windows.
  نبود اطلاعات یعنی `unknown`، نه SSD فرضی.
- background jobs حداکثر ۴ برای SSD گزارش‌شده و ۲ برای rotational/unknown، و همیشه حداکثر CPU budget توصیه‌شده‌اند.
  این نسخه oversubscription را کم می‌کند؛ تعداد زیاد core به‌تنهایی دلیل افزایش تهاجمی compaction نیست.
- observed/effective policy در logger `nexoradb_admin.native` با سطح INFO ثبت می‌شود؛ برای نگهداری شواهد logger را در INFO فعال کنید.
  probe مستقل زیر نیز همان محاسبه را بدون بازکردن DB چاپ می‌کند.

## کنترل و مشاهده

متغیرها برای **پردازش سرویس جدید** خوانده می‌شوند؛ پردازش درحال‌اجرا یا سرویس سیستم تغییر داده نشده است.

```text
NEXORADB_ROCKS_AUTO_TUNE=true|false    # پیش‌فرض true؛ فقط سیاست startup سرویس
NEXORADB_ROCKS_MEMORY_MB=256          # سقف cache+memtable، نه کل RAM پردازش
NEXORADB_ROCKS_CACHE_MB=0             # صفر یعنی نصف بودجهٔ effective
```

گزارش read-only در PowerShell، بدون benchmark یا ساخت DB:

```powershell
wsl -d Ubuntu -- /home/parham/NexoraDB/build/release-0.4.1-install-test/bin/python /home/parham/NexoraDB/NexoraDB/src/nexoradb_admin/hardware.py --db-path /home/parham/NexoraDB/nexoradb-data
```

به‌جای مسیر نمونه، **مسیر استقرار واقعی DB** را بدهید. برای محاسبه با بودجهٔ دلخواه از `--memory-mb` و `--cache-mb` استفاده کنید.
فرمان مستقیم فایل عمداً نیاز به import package/بازکردن app ندارد؛ از root دارای extension هم‌نام نیز قابل اجراست.

غیرفعال‌کردن سیاست سرویس با `false`، بودجه‌های قبلی صریح را حفظ می‌کند؛ رفتار خودکار affinity در native همچنان برقرار است.
اگر برنامه مستقیماً `DocEngine` را می‌سازد یا lab adapter تنظیمات خودش را می‌دهد، سیاست Python سرویس روی آن اعمال نمی‌شود؛
برای آن deployment نتیجهٔ `detect/recommend` را صریح به `RocksSettings` بدهید و در manifest ثبت کنید.

## محدودهٔ صحت و محدودیت

WAL، sync/ACK، durability، checksum، ایندکس‌ها، snapshot/lease و مجوزها تغییر نمی‌کنند.
Direct I/O، huge pages، NUMA pinning، sysctl و تنظیم سیستم‌عامل خودکار فعال نمی‌شوند.
فقط مسیرهای read-only سیستم خوانده می‌شوند؛ شبکه، subprocess probe، benchmark startup یا root لازم نیست.

بودجهٔ RocksDB سقف RSS نیست: graph، Python، allocator، snapshot retention و buffer شبکه خارج از آن‌اند؛
WBM نیز hard RSS quota نیست. کنترل RSS همچنان به بودجهٔ کل deployment/cgroup و تست حافظه نیاز دارد.
cgroup v1 و والدهای پنهان‌شده در namespace پوشش داده نشده‌اند و scope گزارش این محدودیت را صریح می‌گوید.
سیاست runtime retuning ندارد؛ تغییر quota/affinity پس از بازشدن موتور خودکار تنظیمات قبلی را بازنویسی نمی‌کند.
SSD SATA/NVMe، NUMA و چندموتوری به calibration workload و budget مشترک جدا نیاز دارند.

## شاهد این ماشین و آزمون

probe این نوبت ۳۲ CPU منطقی مجاز و حدود ۳۱٫۲ GiB حافظهٔ قابل مشاهده در WSL گزارش کرد؛
این اعداد توصیف کل RAM/CPU فیزیکی Windows نیستند. دستگاه مهمان پرچم rotational داشت؛
در نتیجه سیاست محافظه‌کارانهٔ دو background job حفظ شد. با بودجهٔ پیش‌فرض، ۲۵۶ MiB موتور و
۱۲۸ MiB cache حفظ شدند؛ به‌صرف آزادبودن RAM، حافظه افزایش نیافت.

تست‌ها شامل والد محدودکنندهٔ cgroup، mount root داخل namespace، سهمیهٔ fractional، headroom صفر،
دادهٔ ناموجود، cache صریح ناسازگار، دیسک گزارش‌شده، اعمال/خاموشی سیاست و validation متغیر محیط‌اند.
تست native affinity را فقط برای thread آزمایشی موقتاً به یک CPU محدود و در پایان بازیابی می‌کند.
این‌ها correctness policy را می‌سنجند، نه درصد افزایش throughput.

برای رقابت پژوهشی، گام بعدی اجرای کنترل A/A و سپس A/B روی workload و سخت‌افزار ثابت است؛
enabled/effective policy باید در شواهد ثبت شود. نتایج فعلی رقبا با این تنظیمات دوباره برچسب نمی‌خورند.
هیچ تست بزرگ/چندساعته، restart سرویس یا commit در این تغییر انجام نشده است.

نتیجهٔ تست همین نوبت: **۶۵ تست native موفق**، **۴۲ تست Python برای hardware/performance/API/ACL/memory موفق**
و **۶ تست pagination موفق**. یک هشدار deprecation موجود Starlette/httpx ثبت شد.
پس از rebuild، نسخهٔ package و extension ریشه همگام و آزمون‌ها در process تازه اجرا شدند؛
آزمون اولیهٔ پیش از همگام‌سازی به‌درستی binary متفاوت را رد کرد و این محافظ loader حذف نشد.
تمام منابع شمارهٔ نسخه همچنان ۰٫۴٫۱ هستند. شواهد تاریخی COR-002 و A/A، متعلق به build زمان خودشان باقی می‌مانند.
