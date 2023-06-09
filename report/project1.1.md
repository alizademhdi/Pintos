<div dir="rtl">

تمرین گروهی ۱/۱ - برنامه‌های کاربر 
======================

شماره گروه: 7
-----
> نام و آدرس پست الکترونیکی اعضای گروه را در این قسمت بنویسید.

سروش شرافت sorousherafat@gmail.com 

علی‌پاشا منتصری alipasha.montaseri@gmail.com

کیان بهادری  kkibian@gmail.com

مهدی علیزاده alizademhdi@gmail.com 


## تغییرات طراحی

 یک‌سری تغییرات در داده‌ساختارها داشتیم که در زیر آورده شده‌اند.

 در داده ساختار `process_status،` یک bool به نام `exited` اضافه کردیم که وضعیت ترد را از نظر اینکه exit شده است یا نه نگه می‌دارد. به کمک بررسی این مقدار، در صورت اینکه ترد مربوطه پیشتر exit شده باشد، کد مناسب (1-) بازگردانده می‌شود. این مورد در پاس شدن تست‌هایی همچون `wait-bad-pid` موثر بود.

در پیاده‌سازی قسمت پاس دادن آرگومان‌های خط فرمان هم با 
چالش‌هایی مواجه شدیم که اکثرا با 
align
نبودن 
`esp` 
به وجود آمده بود ولی در نهایت طبق
همان
8086 Calling Convention
داخل تابع
`setup_stack`
پاس دادن آرگومان‌ها را پیاده‌سازی کردیم. طبق
این convention
ابتدا مقادیر
`argv`
از زیاد به کم داخل استک اضافه می‌شوند، سپس باید 
`esp`
را طوری align 
کنیم که مضرب 4 باشد و سپس آدرس
این
`argv[i]`
ها باید در استک اضافه شود و سپس باید
`esp`
را طوری 
align
کنیم که رقم انتهایی هگز آن
8
باشد و سپس به ترتیب آدرس
`argv`
و 
`argc`
و یک آدرس فیک داخل آن بگذاریم که در نهایت 
رقم آخر
`esp`
به توجه به
align
هایی که انجام شد
مقدار
C
را خواهد داشت.

 یکی از مواردی که در پروژه نیاز به توجه ویژه داشت، تخصیص و  آزادسازی منابع در زمان مناسب بود. برای این کار استراکت `process_status` که برای هر ترد تعریف می‌شود را در ابتدای فراخوانی `process_execute` صدا می‌کنیم تا یک استراکت جدید برای این ترد بسازد. سپس در صورتی که ترد مورد نظر به هر دلیل ساخته نشد یا به پایان منطقی خود رسید، با استفاده از تابع `free_process_resources`
منابع مورد استفاده آن را آزاد می‌کنیم تا memory leak رخ ندهد.


به‌علاوه یک داده ساختار جدید به اسم ts اضافه کردیم، این داده ساختار به این علت اضافه شد که نیاز داشتیم استراکت `process_status` ساخته شده در تابع `process_execute` را به تابع `start_process` منتقل کنیم تا در آنجا `current_thread` را درست تعیین کنیم.


```C

struct ts
{
   char *file_name;
   struct process_status *ps;
};


```

همچنین توجه کنید که نیازی به به داده ساختار FD نیز نداشتیم و توصیف‌گرهای فایل را به صورت یک لیست در داده ساختار thread نگه داشتیم که به صورت زیر پیاده سازی شده است.


```C

struct file * file_descriptors[MAX_OPEN_FILE];

```

نکته‌ی دیگری که لازم به ذکر آن است این است که ۳ خانه‌ی اول این توصیف‌گر ها به ترتیب برای stdin و stdout و stderr رزرو شده اند.

از نکات دیگری که می‌توان اشاره کرد مسئله‌ی چک کردن ولید بودن string ورودی در تابع‌های سیسکال است که در دیزاین اولیه آنرا در نظر نگرفته بودیم و برای پاس کردن تست `exec-missing.c` متوجه این موضوع شدیم و در نهایت. تابع `check_string` را برای حل مشکل پیاده‌سازی کردیم.

همچنین برای چک کردن دسترسی برنامه‌ی کاربر به ورودی‌های توابع سیسکال از `is_user_vaddr` و `pagedir_get_page` استفاده کردیم.

از نکات دیگری که در طراحی اولیه به آن اشاره نکردیم و در تست‌های rox با آن مواجه شدیم این بود که نباید اجازه‌ی نوشتن برای رو خود فایل اجرایی برنامه کابر را در حین اجرا می‌دادیم و برای این کار از تابع `file_deny_write` به همراه یک طراحی برای حل این مشکل استفاده کردیم و داخل 
thread
که برنامه داخل آن اجرا می‌شود هم 
file
اجرایی را ذخیره کردیم تا در نهایت آن را
ببندیم.

همچنین نکته‌ی عجیبی که به آن برخورد کردیم این بود که بعضی از تست‌های sync بدون اضافه کردن `file_lock` پاس می‌شدمد ولی برای کامل شدن طراحی قبل از انجام هر عملیات بر روی فایل با استفاده از توابع `lock_acquire` و `lock_release` لاک را گرفته و آزاد می‌کردیم تا از race condition جلوگیری کنیم.


## تقسیم کار

در ابتدای هر بخش جلساتی به صورت مجازی یا حضوری برگزار می‌کردیم و درمورد راه‌حل های ممکن بحث می‌کردیم و ساختار کد را به صورت abstract توصیف می کردیم و درمورد ایراداتی که می‌تواند در بخش‌های مختلف به وجود بیاورد تبادل نظر می‌کردیم و در نهایت پروژه‌ را به صورت مجموعه‌ای از تسک‌ها تقسیم کردیم(به صورت کلی به بخش‌های پاس دادن آرگومان، فراخوانی‌های سیستمی مانند exec, wait، فراخوانی‌های فایل مانند open, write, ... و همچنین پیاده‌سازی تابع‌هایی برای استفاده‌ی بهینه از فضای حافظه و ...) و تسک‌ها را بین افراد مختلف پخش کردیم و هر فرد مسئول انجام آن بخش بود.

در حین انجام تسک‌ها نیز به یکدیگر آپدیت می‌دادیم و تقریبا به صورت روزانه تعدادی تسک را انجام می‌دادیم و درمورد مشکلات مختلف با هم تبادل نظر می‌کردیم. همچنین بعد از انجام هر تسک فرد دیگری کد و همچنین اجرای درست آن قسمت را review می‌کرد. در مواردی نیز در گروه‌های دونفره کد زدیم تا هم بهتر قسمت‌های مختلف کد را متوجه شویم و هم از باگ‌های احتمالی جلوگیری کنیم.

به نظرمان کار تیمی را به صورت مناسبی انجام دادیم و هر کس نیز به مسئولیت‌های خودش به خوبی عمل کرد.
همچنین به خاطر خوانایی کد و کامیت مسیج‌های مناسب تعامل خیلی راحت‌تر شده بود.

یکی از مشکلاتی که با آن مواجه شدیم در ترتیب پیاده‌سازی فیچرها
بود که مثلا برای بسیاری از تست‌ها لازم بود تعدادی از
syscall
ها پیاده‌سازی شده باشند که آن تست پاس شود و زمان زیادی از دیباگ کردن بدون پیاده‌سازی آن syscall
ها تلف شد که باید در فازهای بعدی پروژه به آن دقت بیشتری داشته باشیم.

</div>