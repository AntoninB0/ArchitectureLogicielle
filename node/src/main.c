#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "protocol_icd.h"

typedef enum {
    APPUI = 1,
    TRIPLE_APPUI = 2,
    FIN_ERREUR = 3
} event_t;

typedef enum {
    ETAT_INIT,
    ETAT_RUNNING,
    ETAT_ERROR
} etat_t;

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(button0), gpios);

K_MSGQ_DEFINE(file, sizeof(event_t), 4, 4);
static struct gpio_callback button_cb_data;
#define DEBOUNCE_MS 75
#define FENETRE_CLICS_MS 1000

/* Nombre de clics dans la fenêtre en cours. Partagé entre l'ISR du bouton
 * et l'expiration du timer (deux contextes d'interruption) -> atomique. */
static atomic_t nb_clics;

/* Fin de la fenêtre de 1 s sans 3e clic : on décide "appui simple". */
void fin_fenetre_clics(struct k_timer *timer_id)
{
    atomic_val_t n = atomic_clear(&nb_clics);
    if (n > 0 && n < 3) {
        event_t evt = APPUI;
        k_msgq_put(&file, &evt, K_NO_WAIT);
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
        event_t evt = TRIPLE_APPUI;
        k_msgq_put(&file, &evt, K_NO_WAIT);
    }
}


void led_blink(struct k_timer *timer_id)
{ 
    gpio_pin_toggle_dt(&led);
}
K_TIMER_DEFINE(led_timer, led_blink, NULL);

void fin_erreur(struct k_timer *timer_id)
{
    event_t evt = FIN_ERREUR;
    k_msgq_put(&file, &evt, K_NO_WAIT);
}
K_TIMER_DEFINE(erreur_timer, fin_erreur, NULL);

/* LED : INIT = 0,5 Hz, RUNNING = fixe, ERROR = 5 Hz */
static void appliquer_etat(etat_t etat)
{
    switch (etat) {
        case ETAT_INIT:
            printk("Etat INIT\n");
            k_timer_start(&led_timer, K_MSEC(1000), K_MSEC(1000));
            break;
        case ETAT_RUNNING:
            printk("Etat RUNNING\n");
            k_timer_stop(&led_timer);
            gpio_pin_set_dt(&led, 1);
            break;
        case ETAT_ERROR:
            printk("Etat ERROR\n");
            k_timer_start(&led_timer, K_MSEC(100), K_MSEC(100));
            k_timer_start(&erreur_timer, K_MSEC(1000), K_NO_WAIT);
            break;
    }
}

int main(void)
{   
    etat_t etat = ETAT_INIT;
    etat_t etat_avant_erreur = ETAT_INIT;
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

    printk("Pret : appui simple = etat suivant, triple appui = reset\n");
    appliquer_etat(etat);

    event_t evt;
    while (1) {
        k_msgq_get(&file, &evt, K_FOREVER);
        if (evt == APPUI) {
            printk("Simple appui detecte\n");
        } else if (evt == TRIPLE_APPUI) {
            printk("Triple appui detecte\n");
        }

        if (evt == TRIPLE_APPUI) {
            printk("Reinitialisation du noeud\n");
            k_timer_stop(&erreur_timer);
            etat = ETAT_INIT;
            appliquer_etat(etat);
            continue;
        }

        switch (etat) {
            case ETAT_INIT:
                if (evt == APPUI) {
                    etat = ETAT_RUNNING;
                    appliquer_etat(etat);
                }
                break;
            case ETAT_RUNNING:
                if (evt == APPUI) {
                    etat_avant_erreur = etat;
                    etat = ETAT_ERROR;
                    appliquer_etat(etat);
                }
                break;
            case ETAT_ERROR:
                if (evt == FIN_ERREUR) {
                    etat = etat_avant_erreur;
                    appliquer_etat(etat);
                }
                break;
        }
    }
    return 0;
}
