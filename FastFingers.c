#include <stdio.h>           // Biblioteca padrão de entrada e saída
#include <stdlib.h>          // Biblioteca para manipulação de memória e funções rand()
#include <string.h>          // Biblioteca para manipulação de strings
#include "pico/stdlib.h"     // Biblioteca padrão do Raspberry Pi Pico
#include "hardware/timer.h"  // Biblioteca para manipulação de timers no RP2040
#include "hardware/pwm.h"    // Biblioteca para controle de PWM
#include "hardware/clocks.h" // Biblioteca para manipulação de clocks
#include "hardware/gpio.h"   // Biblioteca para manipulação de GPIOs
#include "hardware/i2c.h"    // Biblioteca para comunicação I2C
#include "inc/ssd1306.h"     // Biblioteca para comunicação com o display OLED
#include "pico/cyw43_arch.h" // Biblioteca para comunicação Wi-Fi
#include "lwip/tcp.h" // Biblioteca para comunicação TCP/IP

// Definição dos pinos utilizados no projeto
#define BUTTON_START 5 // Botão A - Inicia o jogo
#define BUTTON_STOP 6  // Botão B - Captura o tempo de reação
#define LED_GREEN 11   // LED Verde - Indica preparação
#define LED_RED 13     // LED Vermelho - Indica reação
#define BUZZER 21      // Buzzer para emitir som ao acionar o LED vermelho
#define I2C_SDA 14     // Pino SDA para o display OLED
#define I2C_SCL 15     // Pino SCL para o display OLED

#define WIFI_SSID "DANIEL_GOES"  // Nome da rede Wi-Fi
#define WIFI_PASS "daniel007"     //Senha da rede Wi-Fi
#define API_KEY "9BU4SVAIPVBWCZO4" // API Key do ThingSpeak


// Variáveis globais para controle do jogo
bool jogo_em_execucao = false;                  // Indica se o jogo está em execução
bool fase_reacao = false;                // Indica se o jogador deve reagir
absolute_time_t start_time, reaction_time;  // Armazena os tempos de início e reação
volatile bool buzzer_active = false;        // Indica se o buzzer está ativo
volatile bool false_start_detected = false; // Indica se houve uma queima de largada
volatile bool botao_b_pressionado = false;     // Indica se o botão B foi pressionado



static char thingspeak_response[512] = "Aguardando resposta do servidor...";
// Callback para processar a resposta do servidor
static err_t receive_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (!p) {
        tcp_close(tpcb);
        return ERR_OK;
    }

    // Armazena a resposta para exibição no servidor HTTP
    snprintf(thingspeak_response, sizeof(thingspeak_response), "Resposta do ThingSpeak: %.*s", p->len, (char *)p->payload);
    
    // Liberar o buffer e fechar conexão
    pbuf_free(p);
    tcp_close(tpcb);
    return ERR_OK;
}

// Funcao para enviar dados para o ThingSpeak
static void enviar_tempo_para_thingspeak(float time) {
    char post_data[256];

    snprintf(post_data, sizeof(post_data),
             "api_key=%s&field1=%.2f&field2=%.2f",
             API_KEY, time);

    // Criar conexão TCP
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        printf("Erro ao criar PCB TCP\n");
        return;
    }

    // Endereço IP do ThingSpeak (api.thingspeak.com)
    ip_addr_t ip;
    IP4_ADDR(&ip, 184, 106, 153, 149);

    // Conectar ao servidor na porta 80
    err_t err = tcp_connect(pcb, &ip, 80, NULL);
    if (err != ERR_OK) {
        printf("Erro ao conectar no ThingSpeak\n");
        tcp_close(pcb);
        return;
    }

    // Montar a requisição HTTP
    char request[512];
    snprintf(request, sizeof(request),
             "POST /update.json HTTP/1.1\r\n"
             "Host: api.thingspeak.com\r\n"
             "Connection: close\r\n"
             "Content-Type: application/x-www-form-urlencoded\r\n"
             "Content-Length: %d\r\n\r\n"
             "%s",
             (int)strlen(post_data), post_data);

    // Enviar a requisição
    err_t send_err = tcp_write(pcb, request, strlen(request), TCP_WRITE_FLAG_COPY);
    if (send_err != ERR_OK) {
        printf("Erro ao enviar dados para o ThingSpeak\n");
        tcp_close(pcb);
        return;
    } else{
        printf("Dados enviados para ThingSpeak\n");        
    }
    tcp_output(pcb);
    tcp_recv(pcb, receive_callback);
}

void mostrar_texto(const char *text)
{
    struct render_area frame_area = {
        .start_column = 0,
        .end_column = ssd1306_width - 1,
        .start_page = 0,
        .end_page = ssd1306_n_pages - 1};

    calculate_render_area_buffer_length(&frame_area);
    uint8_t ssd[ssd1306_buffer_length];
    memset(ssd, 0, ssd1306_buffer_length);

    int y = 0;
    int line_len = 15;
    char line_buffer[16];
    int text_len = strlen(text);

    for (int i = 0; i < text_len; i += line_len)
    {
        strncpy(line_buffer, text + i, line_len);
        line_buffer[line_len] = '\0';
        ssd1306_draw_string(ssd, 2, y, line_buffer);
        y += 8;
        if (y >= ssd1306_height)
            break;
    }

    render_on_display(ssd, &frame_area);
}
void pwm_init_buzzer(uint pin)
{
    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(pin);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 4.0f);
    pwm_init(slice_num, &config, true);
    pwm_set_gpio_level(pin, 0);
}

int64_t stop_buzzer(alarm_id_t id, void *user_data)
{
    pwm_set_gpio_level(BUZZER, 0);
    buzzer_active = false;
    return 0;
}

void buzzer_beep(uint frequency, uint duration_ms)
{
    if (buzzer_active)
        return;

    uint slice_num = pwm_gpio_to_slice_num(BUZZER);
    uint32_t clock_freq = clock_get_hz(clk_sys);
    uint32_t top = clock_freq / frequency - 1;

    pwm_set_wrap(slice_num, top);
    pwm_set_gpio_level(BUZZER, top / 2);
    buzzer_active = true;

    add_alarm_in_ms(duration_ms, stop_buzzer, NULL, false);
}

/**
 * Inicia o temporizador do jogo, marcando o tempo inicial.
 */
void start_timer()
{
    start_time = get_absolute_time();
}

uint32_t get_elapsed_time()
{
    return absolute_time_diff_us(start_time, reaction_time) / 1000;
}

// Função de debouncing para os botões, evitando leituras múltiplas rápidas.
bool debounce_no_botao(uint gpio)
{
    static uint32_t last_time = 0;
    uint32_t current_time = to_ms_since_boot(get_absolute_time());

    if (current_time - last_time < 50)
    {
        return false;
    }

    last_time = current_time;
    return gpio_get(gpio) == 0;
}

// Inicia uma nova rodada do jogo.
void iniciar_jogo()
{
    if (!jogo_em_execucao)
    {
        jogo_em_execucao = true;
        fase_reacao = false;
        false_start_detected = false;
        botao_b_pressionado = false;
        mostrar_texto("PREPARE-SE...!");

        gpio_put(LED_GREEN, 1);

        uint delay_ms = 1000 + (rand() % 4000);
        for (uint i = 0; i < delay_ms / 10; i++)
        {
            sleep_ms(10);
            if (gpio_get(BUTTON_STOP) == 0)
            {
                false_start_detected = true;
                break;
            }
        }

        if (false_start_detected)
        {
            mostrar_texto("MUITO CEDO!");
            gpio_put(LED_GREEN, 0);
            for (int j = 0; j < 3; j++)
            {
                gpio_put(LED_RED, 1);
                sleep_ms(200);
                gpio_put(LED_RED, 0);
                sleep_ms(200);
            }
            jogo_em_execucao = false;
            fase_reacao = false;
            sleep_ms(2000);
            mostrar_texto("PRESSIONE A    PARA INICIAR!");
            return;
        }

        gpio_put(LED_GREEN, 0);
        gpio_put(LED_RED, 1);
        buzzer_beep(3000, 300);
        start_timer();
        fase_reacao = true;
        mostrar_texto("PRESSIONE B    PARA MARCAR!");
    }
}

/**
 * Callback de interrupção para o botão de parada (B).
 */
void gpio_callback(uint gpio, uint32_t events)
{
    if (gpio == BUTTON_STOP && jogo_em_execucao && fase_reacao)
    {
        reaction_time = get_absolute_time();
        botao_b_pressionado = true;
    }
}

int main()
{
    stdio_init_all();

    i2c_init(i2c1, ssd1306_i2c_clock * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    uint32_t last_request_time = 0;
    // Inicializa o Wi-Fi
    if (cyw43_arch_init()) {
        printf("Erro ao inicializar o Wi-Fi\n");
        return 1;
    }
    sleep_ms(1000); 
    cyw43_arch_enable_sta_mode();
    printf("Conectando no Wi-Fi...\n");

    sleep_ms(1000); 

    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK, 10000)) {
        printf("Falha ao conectar no Wi-Fi\n");
        return 1;
    } else {
        printf("Conectado ao Wi-Fi!\n");
    }

    sleep_ms(5000); // Aguarda a conexão ao Wi-Fi

    ssd1306_init();
    mostrar_texto("PRESSIONE A    PARA INICIAR!");

    gpio_init(BUTTON_START);
    gpio_init(BUTTON_STOP);
    gpio_set_dir(BUTTON_START, GPIO_IN);
    gpio_set_dir(BUTTON_STOP, GPIO_IN);
    gpio_pull_up(BUTTON_START);
    gpio_pull_up(BUTTON_STOP);

    gpio_init(LED_GREEN);
    gpio_init(LED_RED);
    gpio_set_dir(LED_GREEN, GPIO_OUT);
    gpio_set_dir(LED_RED, GPIO_OUT);
    gpio_put(LED_GREEN, 0);
    gpio_put(LED_RED, 0);

    pwm_init_buzzer(BUZZER);

    gpio_set_irq_enabled_with_callback(BUTTON_STOP, GPIO_IRQ_EDGE_FALL, true, &gpio_callback);

    while (true)
    {
        uint32_t current_time = to_ms_since_boot(get_absolute_time()); // Obtém o tempo atual
        if (debounce_no_botao(BUTTON_START))
        {
            if (!jogo_em_execucao)
            {
                iniciar_jogo();
            }
            sleep_ms(300);
        }

        if (jogo_em_execucao && fase_reacao && botao_b_pressionado)
        {
            uint32_t elapsed_time = get_elapsed_time();
            gpio_put(LED_RED, 0);

            stop_buzzer(0, NULL);

            char buffer[20];
            sprintf(buffer, "Tempo: %.1f ms", (float)elapsed_time);
            mostrar_texto(buffer);

        // Envia o tempo de resposta para o ThingSpeak a cada 16 segundos
        if (current_time - last_request_time >= 16000) {
            printf("Enviando tempo de resposta ao ThingSpeak");
            enviar_tempo_para_thingspeak((float)elapsed_time);;
            last_request_time = current_time;
        }
            sleep_ms(5000);

            jogo_em_execucao = false;
            fase_reacao = false;
            false_start_detected = false;
            botao_b_pressionado = false;

            mostrar_texto("PRESSIONE A    PARA INICIAR!");
        }
    }
}