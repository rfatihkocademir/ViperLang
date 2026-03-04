# ViperLang — Örnek Programlar (Example Programs)

> Bu doküman, tasarımı tamamlanan ViperLang sözdiziminin (syntax) **farklı iş alanlarında (domains)** nasıl göründüğünü gösterir.

---

## 1. Minimal Sistem Aracı (CLI Dosya İşleyici)

**Görev:** Bir dizindeki tüm `.log` dosyalarını bul, içindeki "ERROR" satırlarını ayıkla ve tek bir `errors.txt` dosyasına zipleyerek/sıkıştırarak yaz.

```
// @contract
// export_fn: start()

use fs
use regex
use gzip

fn process_logs(dir:s)Res<()> {
    v err_file = fs.open("errors.log.gz" "w")|>gzip.writer?
    
    v files = fs.walk(dir)|>fil(.ends_with(".log"))?
    
    f file in files {
        v content = fs.read(file)?
        // Pipe ile satır bazlı filtreleme ve yazma
        content |> split("\n") 
                |> fil(.starts_with("ERROR"))
                |> each(|line| err_file.write("{line}\n")?)
    }
    
    err_file.close()?
    Ok(())
}

@main fn start() = process_logs("./var/logs")|>unwrap
```

**Token Analizi:** İnanılmaz derecede kısa. `fs`, `regex`, `gzip` standart kütüphanedir, auto-import edilebilir ama açıkça göstermek için `use` eklendi. `|>` (pipe) operatörü veri işleme pipeline'ını muazzam akıcılaştırıyor.

---

## 2. E-Ticaret Sepet Mantığı (Business Logic)

**Görev:** Bir kullanıcının alışveriş sepetini hesapla. Premium kullanıcılara %10 indirim uygula. Eğer toplam tutar $500'ı geçerse kargoyu bedava yap. Yoksa $15 kargo ekle.

```
// @contract
// export_st: Item, Cart
// export_fn: calc_total(Cart, b)->f

#[Eq Ser]
st Item(id:i name:s price:f qty:i) {
    subtotal:f = .price * .qty
}

#[Eq Ser]
st Cart(items:[Item]) {
    items_total:f = .items|>map(.subtotal)|>sum
}

fn calc_total(cart:Cart is_premium:b)f {
    v total = cart.items_total

    // Premium indirimi
    i is_premium: total *= 0.90
    
    // Kargo kuralı
    v shipping = i total > 500: 0.0 e: 15.0
    
    total + shipping
}
```

**Syntax Özellikleri:**
- Computed fields (`subtotal`, `items_total`) objeye entegre, çağırırken `.subtotal` yeterli.
- `i total > 500: 0.0 e: 15.0` yapısı ile inline conditional atama (ternary).
- Explicit `return` yok, son iade (expression) otomatik döner.

---

## 3. Asenkron Gerçek Zamanlı Oyun Sunucusu Bağlantısı

**Görev:** Oyun istemcisinden (client) gelen Socket bağlantısını kabul et. Mesajları dinle ve türüne göre (Move, Attack, Chat) işle.

```
// @contract
// export_en: GameAction
// export_fn: handle_client(Socket)

// Algebraic Data Type (Veri Taşıyan Enum)
en GameAction {
    Move(x:f y:f)
    Attack(target_id:i damage:i)
    Chat(msg:s)
    Ping
}

async fn handle_client(sock:Socket)Res<()> {
    // Mesaj geldiği sürece sonsuz döngü (stream)
    w v msg = sock.recv_json<GameAction>() await? {
        m msg {
            Move(x y) => update_pos(sock.client_id x y)
            Attack(target dmg) => apply_damage(target dmg)
            Chat(s) => broadcast_chat(sock.client_id s)
            Ping => sock.send("Pong") await?
        }
    }
    Ok(())
}
```

**Syntax Özellikleri:**
- Enum Pattern Matching: `m msg { ... }` yapısı, payload çıkarma (destructuring) işlemini tek adımda yapıyor.
- Static Typing on Receive: `.recv_json<GameAction>()` ile JSON ağdan gelir gelmez statik type'a parse (serialize) ediliyor. Tanımsız paket gelirse `?` üzerinden hata fırlatılır.

---

## 4. Gömülü Sistem (IoT - Sensör Okuyucu)

**Görev:** Sıcaklık sensöründen I2C protokolü üzerinden ham byte oku, celcius'a çevir, kritik eşiği geçerse alarm tetikle.

```
// @contract
// export_st: Sensor
// export_fn: monitor(Sensor)

use hardware.i2c

c ADDR:u8 = 0x4A
c CRITICAL_TEMP:f = 85.0

st Sensor(bus:i2c.Bus) {
    fn read_temp()Res<f> {
        // Cihazdan 2 byte oku (u8 array)
        v raw:[u8; 2] = .bus.read(ADDR 2)? 
        
        // Byte katarını sayısına (16-bit) çevir
        v val = (raw[0]<<8) | raw[1]
        
        // Sensör spec'ine göre dönüşüm
        Ok(val|>to_f * 0.0625) 
    }
}

l {
    v temp = sensor.read_temp()?
    i temp > CRITICAL_TEMP: trigger_alarm(temp)
    sleep(1000)
}
```

**Syntax Özellikleri:**
- `u8` (Unsigned 8-bit integer) donanım işlerinde yoğun olarak kullanılıyor.
- `[u8; 2]` yapısı, heap'e (Ram) değil Stack'e kaydedilen **sabit boyutlu** diziyi ifade eder. C performansına eşdeğer hız sağlar. Garbage Collector çalışmaz (ViperLang RC kullanır gerçi).
- Bitwise operasyonları (`<<`, `|`) yerel olarak desteklenir.
