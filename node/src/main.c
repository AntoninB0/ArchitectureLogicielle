/* Partie Zephyr du nœud : LED, bouton, 3 UART, identifiant, aléa.
 * Les interruptions ne font que poster des événements dans une file ;
 * le thread main les traite un par un avec la logique de noeud.c. */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/hwinfo.h>
#include "protocol_icd.h"
#include "noeud.h"

typedef enum {
    EVT_OCTET = 0,
    APPUI = 1,
    TRIPLE_APPUI = 2
} type_evt_t;

typedef struct {
    uint8_t type;    /* type_evt_t */
    uint8_t ligne;   /* ligne_t, pour EVT_OCTET */
    uint8_t octet;
    uint8_t inutilise;
} event_t;

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(button0), gpios);

/* USART1 = PC (console) ; USART2 et USART3 = lignes de chaîne, le sens
 * amont/aval est découvert à l'initialisation */
static const struct device *const uarts[NB_LIGNES] = {
    [LIGNE_PC] = DEVICE_DT_GET(DT_NODELABEL(usart1)),
    [LIGNE_A]  = DEVICE_DT_GET(DT_NODELABEL(usart2)),
    [LIGNE_B]  = DEVICE_DT_GET(DT_NODELABEL(usart3)),
};

/* Assez grande pour plusieurs trames de 26 octets sur les 3 lignes */
K_MSGQ_DEFINE(file, sizeof(event_t), 256, 4);
static struct gpio_callback button_cb_data;
static atomic_t octets_perdus;
#define DEBOUNCE_MS 75
#define FENETRE_CLICS_MS 1000
#define PERIODE_TICK_MS 10

/* Nombre de clics dans la fenêtre en cours. Partagé entre l'ISR du bouton
 * et l'expiration du timer (deux contextes d'interruption) -> atomique. */
static atomic_t nb_clics;

static void poster(type_evt_t type)
{
    event_t evt = { .type = type };
    k_msgq_put(&file, &evt, K_NO_WAIT);
}

/* Fin de la fenêtre de 1 s sans 3e clic : on décide "appui simple". */
void fin_fenetre_clics(struct k_timer *timer_id)
{
    atomic_val_t n = atomic_clear(&nb_clics);
    if (n > 0 && n < 3) {
        poster(APPUI);
    }
}
K_TIMER_DEFINE(clics_timer, fin_fenetre_clics, NULL);

void btn_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    static int64_t last_edge;
    int64_t now = k_uptime_get();

    bool rebond = (now - last_edge) < DEBOUNCE_MS;
    last_edge = now;
    if (rebond || gpio_pin_get_dt(&button) != 1) {
        return;
    }

    atomic_val_t n = atomic_inc(&nb_clics) + 1;
    if (n == 1) {
        /* 1er clic : on ouvre la fenêtre, la décision est prise plus tard */
        k_timer_start(&clics_timer, K_MSEC(FENETRE_CLICS_MS), K_NO_WAIT);
    } else if (n == 3) {
        /* 3e clic avant la fin : triple appui, on annule la fenêtre */
        k_timer_stop(&clics_timer);
        atomic_clear(&nb_clics);
        poster(TRIPLE_APPUI);
    }
}

/* Réception UART : chaque octet part dans la file, avec sa ligne */
static void uart_rx_isr(const struct device *dev, void *user_data)
{
    event_t evt = { .type = EVT_OCTET, .ligne = (uint8_t)(uintptr_t)user_data };

    if (!uart_irq_update(dev)) {
        return;
    }
    while (uart_irq_rx_ready(dev) && uart_fifo_read(dev, &evt.octet, 1) == 1) {
        if (k_msgq_put(&file, &evt, K_NO_WAIT) != 0) {
            atomic_inc(&octets_perdus);
        }
    }
}

void led_blink(struct k_timer *timer_id)
{
    gpio_pin_toggle_dt(&led);
}
K_TIMER_DEFINE(led_timer, led_blink, NULL);

/* ------------------------------------------- services fournis à noeud.c */

static void hw_envoyer(void *ctx, ligne_t ligne, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(uarts[ligne], data[i]);
    }
}

/* LED : INIT = 0,5 Hz, SYNCHRO = 2 Hz, RUNNING = fixe, ERROR = 5 Hz */
static void hw_led(void *ctx, led_mode_t mode)
{
    switch (mode) {
        case LED_INIT:
            k_timer_start(&led_timer, K_MSEC(1000), K_MSEC(1000));
            break;
        case LED_SYNCHRO:
            k_timer_start(&led_timer, K_MSEC(250), K_MSEC(250));
            break;
        case LED_RUNNING:
            k_timer_stop(&led_timer);
            gpio_pin_set_dt(&led, 1);
            break;
        case LED_ERROR:
            k_timer_start(&led_timer, K_MSEC(100), K_MSEC(100));
            break;
    }
}

/* Le F103 n'a pas de générateur matériel : l'aléa vient de l'heure exacte
 * (en cycles CPU à 72 MHz) de chaque événement, imprévisible à l'échelle
 * d'un octet UART ou d'un appui humain. */
static uint32_t entropie;

static void melanger_entropie(void)
{
    entropie = (entropie ^ k_cycle_get_32()) * 1664525u + 1013904223u;
}

static uint32_t hw_aleatoire(void *ctx)
{
    melanger_entropie();
    return entropie;
}

static void hw_vlog(void *ctx, const char *fmt, va_list ap)
{
    printk("[%6u] ", k_uptime_get_32());
    vprintk(fmt, ap);
}

static const noeud_ops_t ops = {
    .envoyer = hw_envoyer,
    .led = hw_led,
    .aleatoire = hw_aleatoire,
    .vlog = hw_vlog,
};

/* ID 32 bits dérivé de l'UID 96 bits (lot, wafer, position X/Y).
 * Les 96 bits sont mélangés (FNV-1a) : sur les clones (CKS32) l'UID n'est
 * pas rangé comme sur un vrai STM32, les octets X/Y seuls peuvent valoir 0. */
static uint32_t lire_id(void)
{
    uint8_t uid[12] = { 0 };
    hwinfo_get_device_id(uid, sizeof(uid));
    printk("UID :");
    uint32_t id = 2166136261u;
    for (int i = 0; i < 12; i++) {
        printk(" %02x", uid[i]);
        id = (id ^ uid[i]) * 16777619u;
    }
    printk("\n");
    return (id == DEFAULT_ID) ? 1 : id;   /* DEFAULT_ID est réservé au PC */
}

static noeud_t noeud;

int main(void)
{
    int ret;

    if (!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&button)) {
        printk("Erreur : GPIO LED ou bouton non prêt\n");
        return 0;
    }

    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        printk("Erreur %d : configuration LED\n", ret);
        return 0;
    }

    ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
    if (ret < 0) {
        printk("Erreur %d : configuration bouton\n", ret);
        return 0;
    }

    ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        printk("Erreur %d : configuration interruption bouton\n", ret);
        return 0;
    }
    gpio_init_callback(&button_cb_data, btn_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);

    for (int l = 0; l < NB_LIGNES; l++) {
        if (!device_is_ready(uarts[l])) {
            printk("Erreur : UART de la ligne %d non prete\n", l);
            return 0;
        }
        uart_irq_callback_user_data_set(uarts[l], uart_rx_isr, (void *)(uintptr_t)l);
        uart_irq_rx_enable(uarts[l]);
    }

    uint32_t id = lire_id();
    entropie = id;
    noeud_demarrer(&noeud, id, &ops, k_uptime_get_32());
    printk("Pret : appui simple = infos, triple appui = reset\n");

    event_t evt;
    while (1) {
        /* réveil au moins toutes les 10 ms pour les timeouts */
        if (k_msgq_get(&file, &evt, K_MSEC(PERIODE_TICK_MS)) == 0) {
            uint32_t now = k_uptime_get_32();
            melanger_entropie();
            switch (evt.type) {
                case EVT_OCTET:
                    noeud_octet(&noeud, (ligne_t)evt.ligne, evt.octet, now);
                    break;
                case APPUI:
                    printk("Simple appui detecte\n");
                    noeud_appui_simple(&noeud, now);
                    break;
                case TRIPLE_APPUI:
                    printk("Triple appui detecte\n");
                    noeud_triple_appui(&noeud, now);
                    break;
            }
        }
        noeud_tick(&noeud, k_uptime_get_32());

        atomic_val_t perdus = atomic_clear(&octets_perdus);
        if (perdus) {
            printk("Attention : %d octets perdus (file pleine)\n", (int)perdus);
        }
    }
    return 0;
}
