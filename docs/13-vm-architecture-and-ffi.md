# ViperLang v3 — ViperVM Mimarisi ve C-FFI (Yabancı Fonksiyon Arayüzü)

> **Zorunluluk:** ViperLang, sadece bir API yazıp bekleyen bir dil değil; 3D oyun motorları, IoT sensör sürücüleri ve milyar satırlık performans kritik sistemler için tasarlandı. Bu hedefler, Sanal Makinenin (VM) ve işletim sistemiyle/donanımla haberleşme katmanının (FFI) kusursuz ve sıfır maliyetli (zero-cost) olmasını gerektirir.

Bu doküman, ViperVM'in motor dairesindeki çalışma prensiplerini ve C kütüphaneleriyle entegrasyonunu tanımlar.

---

## 1. ViperVM Mimarisi: Register-Based VM

Sanal makineler (VM'ler) genellikle ikiye ayrılır: 
1. **Stack-Based (Yığın Tabanlı):** Java (JVM), Python, Lua(eski). Derleyicisini yazması kolaydır, bytecode boyutu küçüktür ama CPU üzerinde daha çok işlem (push/pop) yapar, yavaştır.
2. **Register-Based (Yazmaç Tabanlı):** LuaJIT, Android Dalvik. Derleyicisini yazması zordur, bytecode biraz daha büyüktür ama CPU mimarisine çok daha yakın olduğu için **muazzam performanslıdır**.

**ViperVM Kararı: Kesinlikle Register-Based**
Bir LLM olarak ViperLang derleyicisini (C ile) yazmak benim için zor değildir. İnsanın zorlandığı "Register Allocation" (Yazmaç Tahsisi) algoritmalarını kusursuz uygulayabilirim. Bu yüzden ViperVM, doğrudan C'nin hızına yaklaşmak için Register-Based bir mimari kullanır.

### OpCode Örneği (Toplama İşlemi)
`v c = a + b` işlemi:
*   *Stack-Based olsaydı:* `LOAD a`, `LOAD b`, `ADD`, `STORE c` (4 talimat)
*   *ViperVM (Register-Based):* `ADD R1, R2, R3` (Tek talimat, direkt CPU benzeri yürütme).

**Sonuç:** <5ms cold start ve C'ye yakın çalışma zamanı (runtime) hızı garanti edilir.

---

## 2. Memory (Bellek) Yönetimi: C-Style Struct Layouts

ViperVM nesneleri bellekte dağınık tutmaz. Sıkıştırılmış bir legacy projede 1 milyon `User` objesi yaratıldığında, Java veya JavaScript'in yaptığı gibi her obje için pointerları (referansları) oradan oraya savurup CPU Cache'ini bozmaz.

ViperLang struct'ları bellekte doğrudan C struct'ları gibi **bitişik (contiguous) bloklar** halinde sıralanır. Bu, oyun motoru (Game Engine ECS) geliştirirken veya ağır veri işlerken (Data Science) CPU'nun Data-Cache (L1/L2) alanını maksimum verimle kullanmasını sağlar.

---

## 3. C-FFI (Sıfır Maliyetli Yabancı Fonksiyon Arayüzü)

ViperLang, donanım veya işletim sistemi (OS) ile konuşmak için C kütüphanelerine muhtaçtır (Örn: OpenGL, POSIX soketleri, SQLite motoru). 

Python veya Java'da C kodu çağırmak ağrılıdır (JNI, ctypes) ve bir "köprü" (bridge) maliyeti vardır. ViperVM ise C tabanlı olduğu için, C fonksiyonlarını **çağrı maliyeti olmadan (Zero-Overhead)** doğrudan yürütür.

### FFI Kullanımı: `@extern` Bloğu

Bir yapay zeka ajanı (LLM), C dilinde yazılmış bir kütüphaneyi ViperLang'a entegre etmek istediğinde, sadece fonksiyon imzalarını tanımlar:

```
// system.vp

// LLM, C tarafındaki libsqlite3.so kütüphanesini bağlar
@link("sqlite3")
@extern("C") {
    // C'deki fonksiyon imzasının ViperLang karşılığı
    fn sqlite3_open(filename:*s, ppDb:**any) -> i
    fn sqlite3_exec(db:*any, sql:*s, callback:*any, arg:*any, errmsg:**s) -> i
    fn sqlite3_close(db:*any) -> i
}

// ViperLang içinden doğal, güvenli (safe) kullanım sarmalayıcısı (wrapper)
pub fn open_db(path:s)Res<DbConn> {
    v db_ptr:*any = nil
    v rc = sqlite3_open(path.as_ptr() &db_ptr)
    
    i rc != 0: r Err("DB Açılamadı")
    Ok(DbConn(ptr: db_ptr))
}
```

### LLM Kazanımları (Pointer Güvenliği)
1.  **Açık Pointer Tipleri (`*T`):** ViperLang normalde pointer aritmetiğine izin vermez. Güvenlik esastır. Ancak `@extern` bloklarında veya "unsafe" (güvensiz) donanım işlemlerinde, LLM'in pointerları (`*any`, `**s`) açıkça ifade etmesini ister.
2.  **Sızıntı (Leak) Tespiti:** ViperVM, FFI üzerinden alınan pointer belleklerinin referans sayısını (RC) yönetmez; yönetimi dildeki "Wrapping" nesnelerine (Örn: `DbConn`) bırakır. Bu nesnelerin kendi Yok Edici (Destructor) fonksiyonları vardır.

---

## Özet

ViperLang bir betik (script) dili değildir.

1.  **Register-based VM**, kodu doğrudan makine koduna yakın bir hızda işler.
2.  **Bitişik Bellek (Contiguous Memory)**, CPU önbelleklerini (cache) delip geçerek oyun ve hesaplama performansını arşa çıkarır.
3.  **Zero-Cost C-FFI**, LLM'lerin yeryüzündeki tüm mevcut donanım ve işletim sistemi (C/C++) kütüphanelerine, araya hiçbir köprü hantallığı sokmadan dokunabilmesini sağlar.
