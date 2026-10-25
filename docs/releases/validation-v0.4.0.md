# بررسی محلی نامزد ۰.۴.۰ — ۲۰۲۶-۱۰-۲۳

وضعیت: آماده‌سازی محلی و آزمون حافظهٔ ۱۰M تأیید شدند؛ commit/tag نهایی باز هستند.
این گزارش به snapshot محلی مربوط است، نه به commit منتشرشده.

## شواهد مستقل از آزمون بزرگ

- [x] نسخهٔ CMake، Python، package، vcpkg و frontend روی ۰.۴.۰ هماهنگ شد.
- [x] native با Release و dashboard با Node 24 بازسازی شدند.
- [x] ۱۴۰ تست C++ شامل document/graph/recovery/concurrency پاس شدند.
- [x] ۹۰ تست مرتبط Python/API و نسخه‌گذاری پاس شدند.
- [x] preflight واقعی ۲۰هزار رکورد، دو تکرار، ۲۴ workload، full validation، restoration و cleanup کامل شد.
- [x] منابع دانلود lockfile از mirror سفارشی به registry رسمی npm نرمال شدند؛ dependencyهای سازگار آسیب‌پذیر به‌روز شدند. `npm audit` در زمان بررسی صفر مورد گزارش کرد؛ این گواهی امنیت کامل نیست.
- [x] dashboard پس از اصلاح dependencyها دوباره build شد. هشدار chunk بزرگ و dynamic import غیرمؤثر باقی است؛ build را متوقف نکرد.
- [x] wheel از source staging تازه ساخته شد تا assetهای کهنهٔ cache وارد بسته نشوند.
- [x] auditwheel repair برای manylinux_2_35_x86_64 و twine check پاس شدند.
- [x] نصب در venv تازه با dependencyهای واقعی انجام و verifier از `/tmp` با PYTHONPATH حذف‌شده اجرا شد: `Installed wheel verified: 0.4.0 Release 8.5.3`.
- [x] workflow انتشار برای build/audit داشبورد و bootstrap نسخهٔ pinشدهٔ vcpkg اصلاح شد؛ اجرای remote CI هنوز انجام نشده است.

پوشهٔ artifactها: `build/release-0.4.0-artifacts/`.
بستهٔ معتبر این بررسی فقط داخل `wheelhouse-clean/` است؛ raw و wheelhouse اولیه
آثار مراحل ساخت‌اند و به‌عنوان بستهٔ نهایی انتخاب نشوند.

پیش‌آزمون: `nexora-scale-lab/results/memory10m_20261023_055759-preflight`.
این آزمون ظرفیت و پایداری ۱۰M را تأیید نمی‌کند.

## وضعیت working tree و commit

مبنای بررسی: `0a7f7657aea4aa26a32b5c3d4cd47868be2d6812`.
بخش بزرگی از تغییرات tracked موجود تفاوت line ending داشت؛ با
`git diff --ignore-space-at-eol` تغییرات معنایی جدا بررسی شدند. تغییرات نسخه،
ابزار نسخه‌گذاری، preflight، frontend lockfile، خروجی dashboard و workflow
انتشار جزو نامزد جاری‌اند. فایل‌های جدید اسکریپت/تست/مستند هم باید در بازبینی
commit لحاظ شوند. وضعیت کامل در artifact manifest ثبت می‌شود.

- [ ] commit نامزد نهایی ساخته و بازبینی شود؛ hash مبنا، hash نسخهٔ نامزد نیست.
- [x] نتیجهٔ ۱۰M جدید بررسی شد؛ هش native هر دو تکرار با native نامزد wheel یکسان است.
- [ ] CI remote، از جمله sanitizerهای لازم، برای commit نامزد بررسی شود.
- [ ] پس از تصمیم انتشار، tag و release ساخته شوند.

## فرمان آزمون بزرگ

پذیرش انجام‌شده: `nexora-scale-lab/results/memory10m_20261023_064138`.
هر دو job و ۲۴ workload کامل؛ full integrity/restoration و zero failed operations
تأیید شدند. بیشینهٔ حافظهٔ ثبت‌شدهٔ گروه به‌ترتیب ۴٫۲۱۰ و ۴٫۱۹۳ GiB، افزایش
high/max/OOM/OOM-kill صفر و بیشینهٔ swap حدود ۰٫۰۰۳ GiB بود. زمان بستن موتور
حدود ۱٫۵۴ و ۱٫۱۷ ثانیه و نرخ بارگذاری ۱۲۸٬۸۱۶ و ۱۲۶٬۸۰۹ رکورد/ثانیه بود.
این نتیجه به معنای پذیرش ۱۰۰M یا بسته‌شدن تمام مسائل امنیت/گراف نیست.

هش native مشترک با candidate manifest:
`163e959c6058806b1d83d53d15c2799223117ea94105e8f21c89dc99e31d7236`.
هش سورس suite ثبت‌شدهٔ اجرا:
`4a943fe204ab507877a88d06ab4fc1f851af61fd6e5554fad055a4c637cca3b1`.
manifest و گزارش تاریخی اجرای آزمون تغییر داده نشده‌اند.

```powershell
wsl -d Ubuntu -u root -- bash /home/parham/NexoraDB/nexora-scale-lab/scripts/run_memory_10m.sh --run
```

این فرمان اکنون ابتدا نسخه و API native را بررسی، سپس preflight کوچک را اجرا
می‌کند و فقط با موفقیت آن وارد ۱۰M می‌شود. دو تکرار مستقل با integrity کامل
اجرا می‌شوند. full scanهای مکرر می‌توانند چند ساعت طول بکشند. نتیجه‌های قدیمی
با سورس جدید resume نمی‌شوند. نبود خطا در preflight، تضمین موفقیت مقیاس بزرگ نیست.
