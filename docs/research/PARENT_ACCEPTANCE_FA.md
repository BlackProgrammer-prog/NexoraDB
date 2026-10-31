# پذیرش والدهای RES-004 و RES-005

وضعیت ۲۰۲۶-۱۰-۲۸: زیرساخت و آزمون کوتاه آماده‌اند؛ **دو والد هنوز بازند**.
A/Aهای قبلی نوسان بالاتر از آستانه داشتند. ابزار یا تست مصنوعی به‌تنهایی شاهد تفکیک اثر patch از محیط نیست.
در این نوبت اجرای طولانی انجام نشده است.

## کار تکمیل‌شده

- trace نسخهٔ ۳ با warm-up هم‌زمان روی همان workerها، barrier و شمارش عملیات ثابت؛
  affinity فقط برای پردازش‌های آزمایشی، قبل از ایجاد threadهای موتور اعمال می‌شود.
- telemetry هم‌زمان CPU/IO/memory PSI، load، disk counters، فرکانس و governor CPUهای انتخابی؛
  دادهٔ ناموجود در WSL صریح ثبت می‌شود، نه حدس. این counters متعلق به host هستند، نه هزینهٔ منتسب به موتور.
- حفظ source snapshot و binary واقعی، تطبیق loader، runtime dependencies، seed و ترتیب اجرا؛
  SHA قبل/بعد، خروجی exclusive و checksum نهایی. این‌ها امضای رمزنگاری/اثبات hermetic build نیستند.
- probe مستقیم C++، binding و HTTP واقعی با oracle یکسان، warm-up یکسان و شمارش عملیات؛
  منابع embedded به‌صورت combined و server/client شبکه جدا گزارش می‌شوند. تفاضل p99 هزینهٔ خالص TCP نیست.
- `research_acceptance.py` برای plan/run/analyze، خروجی `acceptance.json` و gates ثابت؛
  هیچ checkbox، نسخه، tag یا commit را خودکار تغییر نمی‌دهد.

## معیار تیک‌زدن

پروفایل `long` شامل A/A با **همان binary در دو arm** است: ۱ و ۱۰ کلاینت، هرکدام پنج block سه‌اجرایی،
هر اجرا ۱۰هزار سند، یک‌میلیون عملیات اندازه‌گیری و ۱۰۰هزار عملیات warm-up.
سپس پنج تکرار مستقل سه‌لایه، هر لایه ۲۰هزار عملیات و ۱۰هزار warm-up انجام می‌شود.
این حجم سند کوچک، آزمون ۱۰M/۱۰۰M نیست؛ تعداد زیادی عملیات durable انجام می‌دهد و ممکن است چند ساعت طول بکشد.

پذیرش نیازمند صحت همهٔ پاسخ‌ها، شواهد کامل و بدون تغییر، کنترل A/A پایدار در هر دو concurrency،
حداقل پنج block، drift و CV throughput حداکثر ۱۰٪، CI95 نسبت A/A شامل ۱ برای throughput و p99،
و CV throughput و p99 هر سه لایه در تکرارها حداکثر ۱۰٪ است. threshold بعد از دیدن نتیجه تغییر نمی‌کند.
پس از پاس و بازبینی شواهد، می‌توان والدهای RES-004/005 را تیک زد؛ این پذیرش زیرساخت است،
نه تأیید H1..H6، نه برتری بر رقبا و نه مجوز انتشار ranking. همبستگی/نوسان حل‌نشده باید گزارش شود.

## فرمان اجرا توسط کاربر در PowerShell

ابتدا کارهای سنگین دیگر، build و benchmark هم‌زمان را متوقف کنید؛ working tree و binary طی اجرا عوض نشوند.
ابزار حداقل ۵ GiB فضای آزاد Linux را بررسی می‌کند؛ **فضای درایو میزبان VHDX ویندوز را نیز جدا چک کنید**.
مسیر venv زیر موجود و با dependencies پروژه، uvicorn و httpx آزمایش شده است. نیاز به root، نصب رقبا
یا تغییر تنظیمات سیستم نیست. CPUهای مجاز نخست (تا چهار CPU منطقی) پیش‌فرض‌اند؛ `--cpus` برای انتخاب صریح موجود است.

فقط نمایش طرح، بدون benchmark:

```powershell
wsl -d Ubuntu -- /home/parham/NexoraDB/build/release-0.4.1-install-test/bin/python /home/parham/NexoraDB/scripts/research_acceptance.py plan --profile long
```

اجرای اصلی؛ پوشه باید **جدید** باشد:

```powershell
wsl -d Ubuntu -- /home/parham/NexoraDB/build/release-0.4.1-install-test/bin/python /home/parham/NexoraDB/scripts/research_acceptance.py run --profile long --directory /home/parham/NexoraDB/build/phase0-parent-acceptance-20261028
```

بازتحلیل همان شواهد، بدون اجرای benchmark تازه:

```powershell
wsl -d Ubuntu -- /home/parham/NexoraDB/build/release-0.4.1-install-test/bin/python /home/parham/NexoraDB/scripts/research_acceptance.py analyze --directory /home/parham/NexoraDB/build/phase0-parent-acceptance-20261028
```

قبل از هر نمونه و پایان آن progress در stderr چاپ می‌شود؛ driver بلند نیز حین warm-up/اندازه‌گیری پیشرفت را در stderr همان نمونه ثبت می‌کند.
فایل‌های telemetry و stdout/stderr جزئیات را نگه می‌دارند. سقف هر process برابر ۶۰۰ ثانیه است؛
`maximum_scheduled_seconds=27200` جمع timeoutها و allowanceهاست، **نه تخمین زمان واقعی یا سقف سراسری controller**.
اگر نمونه timeout شود، اجرای ناقص برای پذیرش معتبر نیست. اجرا resume ندارد؛ شواهد قبلی را نگه دارید و پوشهٔ تازه بسازید.

خروج Python صفر یعنی gates پاس؛ ۲ یعنی اجرای/تحلیل کامل با gates ردشده (یا پروفایل کوتاه)،
سایر خطاها یا نبود completion یعنی پذیرش نامعتبر/ناقص. WSL ممکن است exit code را در shell میزبان نگاشت کند؛
معیار نهایی `acceptance.json`، `completed.json` و `failures` هستند، نه فقط عدد exit در PowerShell.
در صورت شکست gate، علت را با telemetry و همهٔ rawها بررسی کنید؛ بهترین run را cherry-pick نکنید.

## شاهد کوتاه این نوبت

`build/phase0-acceptance-wiring-cor001-20261028/`: هر دو A/A و هر سه لایه تا پایان اجرا شدند،
seal و analyze پاس شدند؛ والدها `false` باقی ماندند. پروفایل کوتاه عمداً برای پذیرش کافی نیست
و نوسان آن نیز واقعی ثبت شده است. registration و baseline قدیمی دست‌نخورده‌اند؛
binary پس از COR-001 rebuild شده و طرح جدید به نتایج قدیمی برچسب تازه نمی‌زند.
