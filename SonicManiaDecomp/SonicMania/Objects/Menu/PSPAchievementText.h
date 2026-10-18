// The PSP achievements page's texts (MainMenu.c), in the engine's language order: each achievement's name and
// description, as UTF-8, in the order the engine registers them (UserCore.cpp). English is the engine's own wording; the
// others are this port's translations, written with the characters the game's fonts have (tools/check_ach_text.py).
#ifndef PSP_ACHIEVEMENT_TEXT_H
#define PSP_ACHIEVEMENT_TEXT_H

#define PSP_ACH_TEXT_COUNT (18)

// "X OF 18": the word between, in the bold letters (which have no slash)
static const char *pspAchCounterWord[] = { "OF", "SUR", "DI", "VON", "DE", "OF", "OF", "OF", "OF", "DE" };

// Which table each language uses: the small font has only the characters the game's own texts need, too few Korean
// ones (about 200 syllables) for these, and too few of the Simplified Chinese ones that differ from the Traditional
// (no 过, 银, 们...): Korean reads the English, Simplified Chinese the Traditional
#define PSP_ACH_TEXT_EN (0)
#define PSP_ACH_TEXT_TC (6)
#define PSP_ACH_TEXT_PT (7) // (Portuguese, the PSP port's own language)
static const int pspAchTextTable[] = { 0, 1, 2, 3, 4, 5, PSP_ACH_TEXT_EN, PSP_ACH_TEXT_TC, PSP_ACH_TEXT_TC, PSP_ACH_TEXT_PT };

static const char *pspAchText[][PSP_ACH_TEXT_COUNT][2] = {
    // EN
    {
        { "No Way? No Way!", "Collect gold medallions in Blue Spheres Bonus stage" },
        { "Full Medal Jacket", "Collect silver medallions in Blue Spheres Bonus stage" },
        { "Magnificent Seven", "Collect all seven Chaos Emeralds" },
        { "See You Next Game", "Achieve any ending" },
        { "Superstar", "Spin the Star Post!" },
        { "That's a Two-fer", "Find the hidden item boxes at the end of the Zone" },
        { "Now It Can't Hurt You Anymore", "What would happen if you cross a bridge with a fire shield?" },
        { "Triple Trouble", "Try for a 3 chain combo!" },
        { "The Most Famous Hedgehog in the World", "Have your photos taken in Studiopolis Zone" },
        { "Window Shopping", "Let the wind take you through" },
        { "Crate Expectations", "Wreak havoc at the propaganda factory" },
        { "King of Speed", "Get through Stardust Speedway Zone as quickly as possible" },
        { "Boat Enthusiast", "We really like boats" },
        { "The Password is \"Special Stage\"", "Try pushing a barrel to see how far it goes" },
        { "Secret Sub", "You might have to submerge to find it" },
        { "Without a Trace", "Barrel through the lava, don't let anything stop you" },
        { "Collect 'Em All", "Gotta gacha 'em all" },
        { "Professional Hedgehog", "That elusive perfect run, only a professional can achieve" },
    },
    // FR
    {
        { "Impossible ? Pas du tout !", "Obtenez les médaillons d'or dans le stage bonus Sphères Bleues" },
        { "Full Medal Jacket", "Obtenez les médaillons d'argent dans le stage bonus Sphères Bleues" },
        { "Les Sept Mercenaires", "Obtenez les sept Émeraudes du Chaos" },
        { "À la prochaine !", "Voyez une des fins du jeu" },
        { "Superstar", "Faites tourner un poteau de contrôle !" },
        { "Deux pour le prix d'un", "Trouvez les boîtes cachées à la fin de la Zone" },
        { "Même pas mal", "Que se passe-t-il si vous traversez un pont avec un bouclier de feu ?" },
        { "Triple menace", "Tentez un combo de 3 !" },
        { "Le hérisson le plus célèbre du monde", "Faites-vous prendre en photo dans Studiopolis Zone" },
        { "Lèche-vitrines", "Laissez le vent vous porter" },
        { "Caisses en folie", "Semez la pagaille dans l'usine de propagande" },
        { "Roi de la vitesse", "Traversez Stardust Speedway Zone aussi vite que possible" },
        { "Fan de bateaux", "On aime vraiment les bateaux" },
        { "Le mot de passe est « Stage Spécial »", "Poussez un tonneau pour voir jusqu'où il va" },
        { "Sous-marin secret", "Il faudra peut-être plonger pour le trouver" },
        { "Sans laisser de trace", "Foncez dans la lave en tonneau, sans vous arrêter" },
        { "Attrapez-les tous", "Obtenez-les tous à la machine gacha" },
        { "Hérisson professionnel", "La course parfaite, que seul un professionnel peut réussir" },
    },
    // IT
    {
        { "Impossibile? Niente affatto!", "Ottieni i medaglioni d'oro nella fase bonus Sfere Blu" },
        { "Full Medal Jacket", "Ottieni i medaglioni d'argento nella fase bonus Sfere Blu" },
        { "I magnifici sette", "Ottieni tutti e sette gli Smeraldi del Caos" },
        { "Alla prossima!", "Raggiungi un finale qualsiasi" },
        { "Superstar", "Fai girare un palo checkpoint!" },
        { "Due al prezzo di uno", "Trova le casse nascoste alla fine della Zona" },
        { "Ora non ti fa più male", "Cosa succede se attraversi un ponte con lo scudo di fuoco?" },
        { "Tripla minaccia", "Prova a fare una combo da 3!" },
        { "Il riccio più famoso del mondo", "Fatti fotografare in Studiopolis Zone" },
        { "Vetrine al vento", "Lasciati trasportare dal vento" },
        { "Casse e caos", "Scatena il caos nella fabbrica della propaganda" },
        { "Re della velocità", "Attraversa Stardust Speedway Zone il più in fretta possibile" },
        { "Appassionato di barche", "Ci piacciono proprio le barche" },
        { "La password è \"Fase Speciale\"", "Prova a spingere un barile per vedere fin dove arriva" },
        { "Sottomarino segreto", "Forse dovrai immergerti per trovarlo" },
        { "Senza lasciare traccia", "Sfreccia nella lava in un barile, senza fermarti" },
        { "Prendili tutti", "Collezionali tutti con il gacha" },
        { "Riccio professionista", "La corsa perfetta, che solo un professionista sa fare" },
    },
    // GE
    {
        { "Unmöglich? Von wegen!", "Sammle Goldmedaillen in der Bonusstufe Blaue Kugeln" },
        { "Full Medal Jacket", "Sammle Silbermedaillen in der Bonusstufe Blaue Kugeln" },
        { "Die glorreichen Sieben", "Sammle alle sieben Chaos Emeralds" },
        { "Bis zum nächsten Spiel", "Erreiche ein beliebiges Ende" },
        { "Superstar", "Dreh einen Checkpoint-Pfosten!" },
        { "Zwei auf einen Streich", "Finde die versteckten Itemboxen am Ende der Zone" },
        { "Jetzt tut es nicht mehr weh", "Was passiert, wenn du mit einem Feuerschild über eine Brücke läufst?" },
        { "Dreifacher Ärger", "Schaffe eine 3er-Kombo!" },
        { "Der berühmteste Igel der Welt", "Lass dich in der Studiopolis Zone fotografieren" },
        { "Schaufensterbummel", "Lass dich vom Wind tragen" },
        { "Kistenchaos", "Richte Chaos in der Propagandafabrik an" },
        { "König der Geschwindigkeit", "Durchquere die Stardust Speedway Zone so schnell wie möglich" },
        { "Bootsfan", "Wir mögen Boote wirklich sehr" },
        { "Das Passwort lautet „Spezialstufe“", "Schieb ein Fass an und schau, wie weit es rollt" },
        { "Geheimes U-Boot", "Vielleicht musst du abtauchen, um es zu finden" },
        { "Spurlos", "Rase im Fass durch die Lava, lass dich von nichts aufhalten" },
        { "Sammle sie alle", "Hol sie dir alle aus dem Gacha-Automaten" },
        { "Profi-Igel", "Der perfekte Lauf, den nur ein Profi schafft" },
    },
    // SP
    {
        { "¿Imposible? ¡Para nada!", "Consigue los medallones de oro en la fase extra Esferas Azules" },
        { "Full Medal Jacket", "Consigue los medallones de plata en la fase extra Esferas Azules" },
        { "Los siete magníficos", "Consigue las siete Esmeraldas del Caos" },
        { "Hasta el próximo juego", "Consigue cualquier final" },
        { "Superestrella", "¡Haz girar un poste de control!" },
        { "Dos por uno", "Encuentra las cajas ocultas al final de la Zona" },
        { "Ya no puede hacerte daño", "¿Qué pasaría si cruzas un puente con un escudo de fuego?" },
        { "Triple amenaza", "¡Intenta un combo de 3!" },
        { "El erizo más famoso del mundo", "Hazte fotos en Studiopolis Zone" },
        { "De escaparates", "Deja que el viento te lleve" },
        { "Cajas y caos", "Siembra el caos en la fábrica de propaganda" },
        { "Rey de la velocidad", "Atraviesa Stardust Speedway Zone lo más rápido posible" },
        { "Fan de los barcos", "Nos encantan los barcos" },
        { "La contraseña es \"Fase Especial\"", "Empuja un barril para ver hasta dónde llega" },
        { "Submarino secreto", "Puede que tengas que sumergirte para encontrarlo" },
        { "Sin dejar rastro", "Cruza la lava en un barril, que nada te detenga" },
        { "Hazte con todos", "Consíguelos todos en la máquina de gacha" },
        { "Erizo profesional", "La carrera perfecta que solo un profesional consigue" },
    },
    // JP
    {
        { "ありえない？ありえる！", "ブルースフィアで金メダルを集めよう" },
        { "フルメタル・ジャケット", "ブルースフィアで銀メダルを集めよう" },
        { "荒野の七人", "カオスエメラルドを7つすべて集めよう" },
        { "また次のゲームで", "いずれかのエンディングを見よう" },
        { "スーパースター", "スターポストを回そう！" },
        { "一石二鳥", "ゾーンの最後に隠されたアイテムボックスを見つけよう" },
        { "もう怖くない", "ファイアバリアで橋を渡るとどうなる？" },
        { "トリプル・トラブル", "3連続コンボを決めよう！" },
        { "世界一有名なハリネズミ", "スタジオポリスゾーンで写真を撮られよう" },
        { "ウィンドウショッピング", "風に身をまかせよう" },
        { "木箱パニック", "プロパガンダ工場で大暴れしよう" },
        { "スピードキング", "スターダストスピードウェイゾーンをできるだけ速く駆け抜けよう" },
        { "ボート好き", "ボートが大好きです" },
        { "パスワードは「スペシャルステージ」", "タルを押して、どこまで行くか試そう" },
        { "秘密の潜水艦", "見つけるには潜る必要があるかも" },
        { "跡形もなく", "タルで溶岩の中を突き進め、何にも止められるな" },
        { "ぜんぶ集めよう", "ガチャでぜんぶ集めよう" },
        { "プロのハリネズミ", "プロにしかできない、完璧な走り" },
    },
    // TC
    {
        { "不可能？沒問題！", "在藍球獎勵關卡中收集金獎章" },
        { "全金屬外殼", "在藍球獎勵關卡中收集銀獎章" },
        { "豪勇七蛟龍", "收集全部七顆混沌翡翠" },
        { "下次遊戲再見", "達成任意結局" },
        { "超級巨星", "轉動星星柱！" },
        { "一舉兩得", "找到區域盡頭隱藏的道具箱" },
        { "現在它傷不到你了", "帶著火之護盾過橋會發生什麼？" },
        { "三重麻煩", "嘗試三連續組合！" },
        { "世界上最有名的刺蝟", "在Studiopolis Zone拍照" },
        { "隨風購物", "隨風而行" },
        { "箱子大亂鬥", "在宣傳工廠裡大鬧一場" },
        { "速度之王", "盡快通過Stardust Speedway Zone" },
        { "船隻愛好者", "我們真的很喜歡船" },
        { "密碼是「特別關卡」", "推動木桶，看看它能走多遠" },
        { "秘密潛艇", "也許你得潛下去才能找到" },
        { "無影無蹤", "坐著木桶衝過岩漿，別讓任何東西阻止你" },
        { "全部收集", "用轉蛋機全部收集" },
        { "專業刺蝟", "只有專業選手才能完成的完美通關" },
    },
    // PT
    {
        { "Impossível? Que nada!", "Pegue as medalhas de ouro na fase bônus Blue Spheres" },
        { "Full Medal Jacket", "Pegue as medalhas de prata na fase bônus Blue Spheres" },
        { "Sete Homens e um Destino", "Junte as sete Esmeraldas do Caos" },
        { "Até o Próximo Jogo", "Veja qualquer um dos finais" },
        { "Superstar", "Gire um poste de checkpoint!" },
        { "Dois em Um", "Ache as caixas de item escondidas no fim da Zona" },
        { "Agora Nem Dói", "O que acontece se você cruzar uma ponte com o escudo de fogo?" },
        { "Tripla Encrenca", "Tente fazer um combo de 3!" },
        { "O Ouriço Mais Famoso do Mundo", "Seja fotografado na Studiopolis Zone" },
        { "Vitrine ao Vento", "Deixe o vento te levar" },
        { "Caixas e Caos", "Cause estrago na fábrica de propaganda" },
        { "Rei da Velocidade", "Passe pela Stardust Speedway Zone o mais rápido possível" },
        { "Louco por Barcos", "A gente gosta muito de barcos" },
        { "A Senha é \"Fase Especial\"", "Empurre um barril para ver até onde ele vai" },
        { "Submarino Secreto", "Talvez você precise mergulhar para achar" },
        { "Sem Deixar Rastro", "Atravesse a lava num barril, sem deixar nada te parar" },
        { "Pegue Todos", "Colecione todos na máquina de gacha" },
        { "Ouriço Profissional", "A corrida perfeita, que só um profissional consegue" },
    },
};

#endif // !PSP_ACHIEVEMENT_TEXT_H
