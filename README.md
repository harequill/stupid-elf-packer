# stupid-elf-packer

Um *packer* de ELF propositalmente simples, feito como prova de conceito. O objetivo não é ser robusto nem esperto. O objetivo é ser didático: mostrar, com o mínimo de código possível, o que acontece quando um executável é "empacotado" e depois desempacotado e executado em tempo de execução.

## O que é um packer?

Um packer pega um executável original, guarda ele (comprimido e/ou codificado) dentro de um novo executável e coloca junto um pequeno programa chamado **stub**. Quando você roda o binário empacotado:

1. O sistema operacional carrega e executa o **stub**, não o programa original.
2. O stub recupera o programa original que estava guardado (aqui, de forma bem ingênua, daí o "stupid").
3. O stub devolve o controle para o **entry point** original, e o programa roda como se nada tivesse acontecido.

Packers têm usos legítimos (redução de tamanho, proteção de propriedade intelectual) e são um exemplo excelente para ensinar **formato ELF**, **loader** e **layout de memória de um processo**.

## Estrutura do repositório

```
target/   Programa "alvo": um executável bobo que só imprime um texto.
stub/     O stub que roda primeiro e devolve o controle ao alvo original.
tool/     A ferramenta que junta alvo + stub e gera o binário empacotado.
shared/   Código/definições compartilhadas entre stub e tool.
```

## O alvo (target)

O alvo é o programa mais bobo possível, só para ter algo para empacotar:

```c
#include <stdio.h>

int main(void) {
    printf("hello, i am the packer target");
    return 42; // because that's the answer to everything, right?
}
```

Compilação:

```sh
gcc -static -no-pie -o target/target target/target.c
```

## Por que `-no-pie`?

Essa é a decisão de projeto mais importante do exemplo, então vale explicar com calma.

### Com `-no-pie` (o que usamos)

Com `-no-pie`, o binário é carregado sempre no mesmo endereço virtual fixo. Isso significa que o **entry point original é uma constante**: dá para saber exatamente onde ele está já na hora de empacotar.

Consequência: o stub devolve o controle ao programa original com **um único `jmp` para um endereço fixo**. Nada de cálculo, nada de descoberta em runtime. O stub fica o mais simples possível, que é justamente a intenção da POC.

### Com PIE (o que quebraria)

Um executável PIE (*Position Independent Executable*) é reposicionável: por causa do **ASLR**, o loader coloca a imagem numa base diferente a cada execução.

Um `jmp` com endereço "chumbado" no código quebra nesse cenário: na próxima execução o entry point original não está mais naquele endereço.

Para suportar PIE, o stub precisaria, em tempo de execução:

1. descobrir onde ele próprio foi carregado,
2. calcular a base real da imagem, e
3. ajustar (relocar) todos os endereços relativos a essa base.

Esse trabalho de relocação em runtime está fora do escopo deste exemplo. Ele é um ótimo "próximo passo" para quem quiser evoluir a POC, mas adicioná-lo agora só esconderia a ideia central atrás de complexidade desnecessária.

> **Resumo:** `-no-pie` significa endereço fixo, e o stub faz um `jmp` simples.
> PIE significa endereço mudando a cada execução, o que exigiria relocação em runtime.

## Por que `-static`?

Além do endereço fixo, também queremos um fluxo de inicialização previsível, e é por isso que compilamos com `-static`.

Num binário dinâmico, o kernel não entrega o controle direto ao programa. O ELF carrega um campo `PT_INTERP` apontando para o *dynamic linker* (por exemplo `/lib64/ld-linux-x86-64.so.2`), e é ele quem roda primeiro: mapeia as bibliotecas compartilhadas, resolve símbolos e só depois passa o controle ao programa. Isso é uma camada a mais entre "o kernel carregou o binário" e "o meu código roda".

Com `-static`, não há `PT_INTERP` nem loader dinâmico envolvido. O kernel carrega a imagem e entrega o controle **direto ao `_start`**. O fluxo é previsível e inteiramente contido no próprio binário, que é exatamente o que queremos para raciocinar sobre o packer sem a dor de cabeça de lidar com o loader dinâmico.

> **Resumo:** `-static` significa sem `PT_INTERP` e sem dynamic linker.
> O controle vai direto para o `_start`, num fluxo previsível.

## Inspecionando o alvo

Antes de empacotar qualquer coisa, precisamos saber ler a estrutura do binário. As ferramentas abaixo (`file` e `readelf`) mostram tudo o que o packer vai precisar.

> **Por que o binário `target/target` está versionado no repositório?**
> Normalmente não se commita artefato de build, mas aqui é proposital. Os valores mostrados nas saídas abaixo (entry point, offsets e tamanhos dos segmentos, BuildID) são fixos para *este* binário, mas dependem do toolchain que o gerou: recompilar com outra versão de gcc/ld/glibc, com outras flags ou com outro linker pode deslocar o layout e mudar esses números. Versionar o binário garante que, ao rodar os comandos deste README, você veja exatamente os mesmos dados aqui documentados. (Os comandos `file`/`readelf` só *leem* o binário; quem muda os valores é a recompilação, não a execução deles.)

### `file target`

```
ELF 64-bit LSB executable, x86-64, version 1 (GNU/Linux), statically linked, BuildID[sha1]=d4980fe039ab762df9db4ce13f31f19c08a5217a, for GNU/Linux 4.4.0, not stripped
```

Confirma o que compilamos: é um executável ELF de 64 bits e está **`statically linked`** (efeito do `-static`). Ou seja, sem dependência de loader dinâmico, como discutido acima.

### `readelf -h target` (cabeçalho ELF)

```
ELF Header:
  ...
  Type:                              EXEC (Executable file)
  Machine:                           Advanced Micro Devices X86-64
  Entry point address:               0x402e40
  Start of program headers:          64 (bytes into file)
  Number of program headers:         12
  ...
```

Dois campos interessam ao packer:

- **`Type: EXEC`** confirma que é um executável de endereço fixo (efeito do `-no-pie`). Se fosse PIE, apareceria `DYN`.
- **`Entry point address: 0x402e40`** é exatamente o endereço fixo para onde o stub vai dar o `jmp` no final. Como é `-no-pie`, esse valor é constante e conhecido na hora de empacotar.

### `readelf -l target` (program headers / segmentos)

Os *program headers* descrevem os segmentos que o kernel mapeia na memória. Os que importam:

```
  Type           VirtAddr           FileSiz            MemSiz             Flags
  LOAD           0x0000000000400000 0x0000000000000518 0x0000000000000518 R
  LOAD           0x0000000000401000 0x000000000007e7fd 0x000000000007e7fd R E
  LOAD           0x0000000000480000 0x00000000000342f8 0x00000000000342f8 R
  LOAD           0x00000000004b5168 0x00000000000058b0 0x000000000000b180 RW
  NOTE           0x00000000004002e0 0x0000000000000024 0x0000000000000024 R
  ...
```

Dois pontos centrais do plano do packer:

- **O segmento `LOAD` com flags `R E`** (`0x401000`) é o **código** (as seções `.init .plt .text .fini`). É esse segmento que será **cifrado** pelo packer. No binário empacotado ele fica ilegível até o stub decifrá-lo em runtime.
- **Um segmento `NOTE`** (por exemplo o `.note.gnu.build-id`) é apenas metadado descartável para a execução. O plano é **canibalizar esse `NOTE`**, reescrevendo a entrada do program header para transformá-lo num `PT_LOAD` que carrega o **stub**. Assim adicionamos o stub sem precisar realocar a tabela de program headers inteira.

### Executando o alvo

```sh
./target/target ; echo $?
```

```
hello, i am the packer target42
```

Sai o texto definido no `printf` e, logo em seguida, `42` (o `echo $?` mostra o código de saída, que é o `return 42` do `main`). Esse é o comportamento de referência: depois de empacotar e desempacotar, o binário tem que produzir **exatamente** essa mesma saída e esse mesmo código de retorno.

## A ferramenta (tool)

A `tool` é o programa que empacota o alvo: ela lê o ELF, cifra o segmento de código e injeta o stub. Ela é construída em passos, e cada passo é documentado aqui conforme entra.

### Estruturas do ELF que interessam

Um ELF de 64 bits, no que importa para este passo, tem três estruturas, todas definidas em `elf.h`:

- **`Elf64_Ehdr`** (ELF header): fica no offset `0` do arquivo. Ele dá o entry point original (OEP) e diz onde a tabela de program headers começa (`e_phoff`), quantas entradas ela tem (`e_phnum`) e o tamanho de cada entrada (`e_phentsize`).
- **`Elf64_Phdr`** (program header): cada um descreve um segmento. Os campos que usamos: `p_type` (é `PT_LOAD`? `PT_NOTE`?), `p_flags` (tem o bit de execução?), `p_offset` (onde no arquivo), `p_vaddr` (onde na memória), `p_filesz` e `p_memsz` (tamanhos).
- **A tabela de program headers** é só um array de `Elf64_Phdr` em sequência, começando no offset `e_phoff`.

### Passo 1: validar que a entrada é um ELF

Antes de interpretar qualquer campo, a tool confere o **magic number**: os primeiros bytes do arquivo têm que ser `0x7F` seguido de `'E'`, `'L'`, `'F'`. Se não bater, ela aborta com uma mensagem clara e retorna código de erro. Isso evita rodar em cima de lixo e interpretar bytes aleatórios como se fossem um ELF.

Na prática, a tool lê um `Elf64_Ehdr` a partir do offset `0` e compara os primeiros `SELFMAG` (4) bytes de `e_ident` com a constante `ELFMAG` (`"\177ELF"`), ambas de `elf.h`:

```c
if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
    /* not an ELF: abort */
}
```

Também tratamos dois erros de borda antes disso: arquivo que não abre e arquivo pequeno demais para conter um header inteiro.

Compilação:

```sh
gcc -Wall -Wextra -o tool/tool tool/tool.c
```

Uso:

```sh
./tool/tool target/target
# ok: 'target/target' is a valid ELF

./tool/tool arquivo-qualquer
# error: 'arquivo-qualquer' not an ELF (invalid magic number)
```

### Passo 2: ler o ELF e localizar os segmentos

No passo 1 lemos o arquivo com `fopen`/`fread` de propósito, porque mostra de forma explícita a leitura dos bytes do header. A partir daqui a tool passa a usar **`mmap`** do arquivo inteiro: o arquivo é mapeado na memória e tratado como um array de bytes, com ponteiros casteados direto em cima das structs (`Elf64_Ehdr`, `Elf64_Phdr`). É mais confortável e é exatamente como vamos querer trabalhar na hora de reescrever bytes na etapa de cifragem.

Com o arquivo mapeado, a tool faz o seguinte:

**Confirma as premissas da v1.** Lê a classe em `e_ident[EI_CLASS]` (tem que ser `ELFCLASS64`) e o `e_type`. Só aceitamos `ET_EXEC`. Se vier `ET_DYN`, o binário é PIE, e como o stub da v1 não faz relocação em runtime (ver a seção do `-no-pie`), a tool falha aqui com uma mensagem honesta em vez de produzir algo quebrado lá na frente.

**Localiza a tabela de program headers.** Do header já temos `e_phoff` (onde a tabela começa), `e_phnum` (quantas entradas) e `e_phentsize` (tamanho de cada entrada). A tool avança de entrada em entrada usando `e_phentsize` como passo, em vez de assumir `sizeof(Elf64_Phdr)`, para confiar no que o header declara. Antes de ler, confere que a tabela cabe dentro do arquivo.

**Itera e imprime cada program header.** Para cada entrada mostra `p_type`, `p_offset`, `p_vaddr`, `p_filesz`, `p_memsz` e as flags `R`/`W`/`E`. O objetivo é essa saída bater com o `readelf -l`, que é o nosso gabarito.

**Marca os dois segmentos que interessam:**

- O **`PT_LOAD` com o bit de execução (`PF_X`)** é o segmento de **código**. A região `p_offset .. p_offset + p_filesz` dele é o que será cifrado na etapa seguinte.
- O **`PT_NOTE`** é o candidato a ser canibalizado e virar o `PT_LOAD` do stub mais adiante. Por enquanto só confirmamos que existe.

**Imprime um resumo** com o OEP (`e_entry`), o índice/offset/tamanho do segmento de código e se achou o `PT_NOTE`.

Exemplo de saída para o nosso alvo:

```
program headers (12 entries):
idx type         offset             vaddr              filesz             memsz              flg
  0 LOAD         0x0000000000000000 0x0000000000400000 0x0000000000000518 0x0000000000000518 R
  1 LOAD         0x0000000000001000 0x0000000000401000 0x000000000007e7fd 0x000000000007e7fd R E
  ...

=== summary ===
OEP (e_entry):   0x402e40
code segment:    index 1, offset 0x1000, size 0x7e7fd (filesz)
PT_NOTE:         found at index 4
```

#### Como validar

Basta comparar com o `readelf` ao lado:

```sh
readelf -h target/target | grep 'Entry point'   # OEP tem que bater
readelf -l target/target                          # segmentos e o R E têm que bater
```

Se os três dados conferem (entry point igual ao do `readelf -h`, o segmento `R E` igual ao que a tool marcou como código, e os program headers batendo com o `readelf -l`), a leitura do formato está dominada e a base para as próximas etapas está pronta.

## Aquisição de controle: o stub identidade

Aqui vem a parte contra-intuitiva, então vale explicar antes de qualquer código.

O instinto é começar pela criptografia, afinal "empacotar" soa como "cifrar". Mas a gente faz o contrário: primeiro constrói o mecanismo de aquisição de controle **sem cifrar nada**. O alvo continua em plain-text. O objetivo desta fase é provar, isoladamente, que a tool consegue desviar o fluxo de execução para o stub e devolvê-lo limpo para o alvo. Só depois disso a criptografia entra por cima de um esqueleto que já sabemos que funciona.

Se a gente misturasse as duas coisas de uma vez e o binário empacotado quebrasse, não saberíamos se o problema está na cifragem ou na aquisição de controle. Separando, cada fase é depurável sozinha.

A fase se divide em duas partes: o stub e a modificação que a tool faz no binário para dar controle a ele.

### Parte 1: o stub mais burro possível

O stub identidade ganha o controle e imediatamente salta para o OEP, sem fazer mais nada. Se o alvo roda normalmente com esse stub no caminho, está provado que o desvio e o retorno funcionam. É o "hello world" da aquisição de controle.

Ele está em `stub/stub.s`, em assembly AT&T, e é literalmente isto:

```asm
_start:
    movabs $0x402e40, %rax   /* OEP of target/target */
    jmp    *%rax
```

Dois detalhes importantes:

- **Ele não toca na pilha.** Nada de `push`, `call` ou mexer em registradores que o `_start` do alvo espera. Quando o kernel entrega o controle ao entry point, a pilha já está montada com `argc`/`argv`/`envp`/`auxv`. Como o stub só faz `movabs` + `jmp`, o `_start` do alvo recebe a pilha intacta, como se tivesse ganho o controle direto do kernel.
- **O salto é absoluto**, o que torna esses bytes independentes de onde o stub for carregado na memória.

Por enquanto o OEP `0x402e40` está chumbado no stub. Na evolução natural, a tool passaria a patchar esse valor a partir do `e_entry` que ela já lê, deixando o stub genérico.

Montagem do stub (não há `nasm` aqui; usamos o GNU `as` mais `objcopy` para extrair os bytes crus):

```sh
as stub/stub.s -o stub/stub.o
objcopy -O binary --only-section=.text stub/stub.o stub/stub.bin
```

O resultado é um blob de 12 bytes (`48 b8 40 2e 40 00 00 00 00 00 ff e0`) que a tool vai injetar.

### Parte 2: a tool injeta o stub e desvia o entry point

Até aqui a tool só lia o ELF. Agora ela passa a escrever, produzindo uma cópia empacotada (o alvo original é preservado):

```sh
./tool/tool target/target stub/stub.bin target/packed
```

O que ela faz, em ordem:

1. **Anexa o blob do stub no fim do arquivo.** Um append simples dos bytes do stub ao final do binário, guardando em que offset ele caiu.
2. **Converte o `PT_NOTE` em `PT_LOAD`.** Aquele program header `PT_NOTE` que localizamos no passo 2 vira um segmento carregável apontando para o stub. A tool reescreve os campos dele: `p_type` para `PT_LOAD`, `p_flags` com o bit de execução ligado (`R E`), e `p_offset`/`p_vaddr`/`p_filesz`/`p_memsz` apontando para onde o stub foi parar. É isso que faz o kernel mapear o stub na memória em runtime. Canibalizar um `PT_NOTE` (metadado descartável para a execução) evita ter que realocar a tabela de program headers inteira.
3. **Redireciona o entry point.** Troca o `e_entry` do header para o endereço de memória onde o stub vai viver. A partir daí, quando o binário roda, o kernel entrega o controle ao stub, e não ao `main`; o stub salta para o OEP chumbado e o alvo executa.

Dois cuidados são o que separam "funciona" de "o kernel recusa carregar":

- **Alinhamento `p_vaddr` versus `p_offset`.** O ELF exige que os dois sejam congruentes módulo o tamanho de página (`0x1000`). A tool escolhe um `p_vaddr` de base alinhado à página e soma `p_offset % página`, garantindo a congruência. Endereços que não respeitam isso fazem o kernel recusar o binário.
- **`p_vaddr` sem colisão.** O endereço onde o stub vai morar precisa ser uma faixa livre, bem acima dos segmentos existentes. A tool calcula o maior `p_vaddr + p_memsz` de todos os `PT_LOAD`, arredonda para a página seguinte e adiciona uma folga. Se caísse em cima de algo já mapeado, quebraria.

#### A prova

```sh
./target/packed ; echo $?
# hello, i am the packer target42

readelf -h target/packed | grep 'Entry point'
# Entry point address: 0x4c2290   (o stub, e não mais 0x402e40)
```

O empacotado produz exatamente a mesma saída e o mesmo código de retorno (`42`) do original, mas agora o controle passou pelo stub antes de chegar ao alvo. Com esse esqueleto provado, a etapa seguinte (cifrar o segmento de código e mandar o stub decifrar antes de saltar) entra sobre uma base confiável.

## Passando o OEP para o stub: bloco de metadados

O stub identidade tinha o OEP chumbado no código. Resolve um alvo só, mas não escala: cada alvo tem um OEP diferente, e logo vamos precisar passar mais coisa (chave, offset e tamanho da região cifrada). O stub precisa receber esses valores em runtime.

O problema central: a tool sabe o OEP (leu do `e_entry`), o stub precisa dele quando roda. Os dois não compartilham memória nem variáveis. O único canal entre eles são os bytes que a tool escreve no arquivo e o stub lê quando executa. Passar o OEP é, então, a tool gravar o valor em algum lugar do binário e o stub saber ir ler exatamente esse lugar. Os dois precisam concordar sobre o lugar.

### A abordagem: bloco de metadados em posição conhecida

A tool anexa, junto do stub, uma pequena estrutura de dados numa posição previsível, e o stub lê de lá. As vantagens:

- separa código de dados: o stub não muda quando o valor muda;
- é escalável: agregar chave e offsets é só adicionar campos na struct;
- a criptografia depois vira só mais campos, sem reescrever o stub.

### A struct compartilhada

Ela vive em `shared/packer.h` e é incluída pela tool. Por ora só carrega o OEP:

```c
struct packer_meta {
    uint64_t oep;
};
```

Por viver no `shared`, tool e stub concordam sobre o layout de bytes. É um campo só, `oep` no offset 0; conforme a struct cresce, o stub passa a ler cada campo pelo seu offset.

### A tool grava a struct junto do stub

Quando a tool anexa o stub no fim do arquivo, logo depois dos bytes do stub ela grava a struct preenchida com o OEP lido do `e_entry`. A posição é fixa e conhecida: imediatamente após o stub. O `PT_LOAD` que mapeia o stub é dimensionado para cobrir stub mais struct, senão a struct cairia fora do que o kernel mapeia e o stub leria memória inválida.

### O stub encontra a struct e lê o OEP

O stub dinâmico está em `stub/stub_dyn.s` (o identidade continua em `stub/stub.s`, os dois executáveis). Em vez do valor chumbado, ele descobre onde a struct está e lê de lá:

```asm
_start:
    lea    meta(%rip), %rax   /* endereço da struct */
    mov    (%rax), %rax        /* meta.oep */
    jmp    *%rax
meta:
```

O truque está no `lea` relativo ao RIP: o label `meta` é o primeiro byte depois do código do stub, e a tool grava a struct exatamente ali. Assim o stub calcula o endereço da struct em runtime a partir de onde ele mesmo foi carregado, sem depender de endereço fixo. Como `meta` fica no fim do código, o deslocamento é resolvido pelo próprio assembler.

### A prova

```sh
# stub identidade (OEP chumbado); o passo anterior continua funcionando
as stub/stub.s -o stub/stub.o
objcopy -O binary --only-section=.text stub/stub.o stub/stub.bin
./tool/tool target/target stub/stub.bin target/packed
./target/packed ; echo $?
# hello, i am the packer target42

# stub dinâmico (lê o OEP da struct)
as stub/stub_dyn.s -o stub/stub_dyn.o
objcopy -O binary --only-section=.text stub/stub_dyn.o stub/stub_dyn.bin
./tool/tool target/target stub/stub_dyn.bin target/packed_dyn
./target/packed_dyn ; echo $?
# hello, i am the packer target42
```

Os dois produzem a mesma saída. A diferença é que o `packed_dyn` tirou o OEP da struct, e não do código. É a base para, na etapa da criptografia, passar a chave e a região cifrada pelo mesmo canal.

## A cifragem: onde o código original some

Esta é a etapa que faz o "packer" ser um packer. Como o canal tool para stub já existe e o sequestro de controle já funciona, cifrar é quase de graça: somar dois campos na struct e chamar uma função a mais dos dois lados.

### XOR: a mesma função cifra e decifra

O XOR é a sua própria inversa: `dado ^ chave ^ chave == dado`. A tool aplica o XOR uma vez (cifra), o stub aplica de novo com a mesma chave (decifra). Por isso a chave precisa trafegar da tool para o stub, e é justamente por isso que a gente construiu o canal de metadados primeiro.

A função de cifra vive em `shared/xor.h`, incluída pelos dois lados. É literalmente a mesma função rodando como operação e como inversa. Se um lado divergisse do outro num detalhe, decifraria errado; manter tudo no `shared/` mata esse risco na raiz.

```c
static inline void xor_apply(uint8_t *data, uint64_t size, uint64_t key) {
    const uint8_t *k = (const uint8_t *)&key;
    for (uint64_t i = 0; i < size; i++)
        data[i] ^= k[i & 7];
}
```

### A struct cresce

O bloco de metadados ganha três campos além do OEP: o endereço da região cifrada em memória, o tamanho dela e a chave. Por estar no `shared/`, os dois lados enxergam o layout novo automaticamente.

```c
struct packer_meta {
    uint64_t oep;
    uint64_t code_addr;
    uint64_t code_size;
    uint64_t key;
};
```

### A tool cifra a região de código

A tool usa o `PT_LOAD` executável que marcou lá na leitura (o segmento `R E`). A região `p_offset .. p_offset + p_filesz` desse segmento é o que ela passa para a função XOR. Depois de cifrar, preenche na struct o endereço em memória (`p_vaddr`), o tamanho e a chave usada. A ordem importa: cifra primeiro, depois grava os metadados com os valores corretos.

### O stub decifra antes de saltar

O stub de cifragem (`stub/stub_crypt.c`) é o primeiro escrito em C, e não em assembly. O motivo é justamente poder incluir a mesma função XOR do `shared/`. Ele é freestanding: sem libc, chamando a syscall crua (número em `rax`, argumentos nos registradores da convenção, instrução `syscall`), no mesmo espírito do salto. A sequência é: lê os metadados, decifra a região de código, salta para o OEP. O código só fica em plain text na memória no instante antes do salto.

#### Cuidado com R^X

O segmento de código está mapeado como read mais execute, sem write. Quando o stub tenta escrever a versão decifrada por cima, o kernel manda `SIGSEGV`: é a proteção de memória funcionando. A solução é o stub chamar `mprotect` para tornar a região gravável antes de decifrar, decifrar, e devolver para `R+X` (não é obrigatório, mas é higiênico) antes de saltar.

O `mprotect` opera em página (4 KB), então o endereço tem que estar alinhado ao início de uma página. A gente alinha o offset para baixo até a fronteira de página e estende o tamanho para cima na mesma conta. Passar um endereço não alinhado dá erro.

#### Cuidado com o estado de registradores

Este é sutil e vale registrar, porque é o tipo de bug que confunde: a cifra funciona, o `main` roda, e mesmo assim o programa quebra.

O `_start` de um alvo não é uma função normal, não assume convenção de chamada. Ele espera o estado cru que o kernel entrega. A ABI x86-64 garante conteúdo definido para dois registradores na entrada do processo: `rsp` (apontando para `argc`, seguido de `argv`, `envp`, `auxv`) e `rdx` (um ponteiro de função de cleanup, o `rtld_fini`, ou zero num binário estático).

O `stub_main`, sendo código C, mexe em registradores livremente. Em particular, as syscalls de `mprotect` usam `rdx` como terceiro argumento (o `prot`), então quando o `stub_main` retorna, `rdx` está sujo com o último `prot`. Se a gente saltar para o OEP com esse `rdx`, o `_start` do glibc lê o valor como `rtld_fini` e registra um ponteiro de lixo como handler de saída. O `main` roda, imprime (para um buffer), faz `return 42`, e no `exit` o handler de lixo é chamado: `SIGSEGV`. Como o `printf` sem `\n` só é descarregado no `exit`, a saída "hello" nem aparece. Dá para ver o `_start` consumindo o `rdx` no disassembly do alvo:

```
402e46:  49 89 d1   mov %rdx,%r9    # rtld_fini vai para r9 e depois ao __libc_start_main
```

A correção é o `_start` do stub salvar o `rdx` do kernel cedo, num registrador callee-saved (`rbx`, que o `stub_main` é obrigado a preservar), e restaurar antes do salto. Sobre o `rsp`: como o `_start` do stub não empilha nada além do que o `call` empilha (e o `ret` já tira), o `rsp` chega ao OEP no valor original do kernel e alinhado a 16, então não precisa de tratamento extra aqui.

### A cifragem é opcional

Para os passos anteriores continuarem executáveis, a cifra só liga quando uma chave é passada no CLI. Sem chave, a tool empacota sem cifrar e os stubs identidade e dinâmico seguem funcionando; os campos novos da struct ficam zerados.

```
./tool/tool <elf>                        inspeciona
./tool/tool <elf> <stub.bin> <out>        empacota, sem cifrar
./tool/tool <elf> <stub.bin> <out> <key>  empacota e cifra
```

### Montando e empacotando

O stub em C é compilado freestanding e extraído como blob flat via um linker script (`stub/stub.ld`) que coloca `_start` primeiro e o label `meta` no fim do código:

```sh
gcc -c -O2 -nostdlib -ffreestanding -fno-pic -fno-stack-protector \
    -fcf-protection=none -fno-asynchronous-unwind-tables \
    -fno-builtin -fno-tree-loop-distribute-patterns \
    stub/stub_crypt.c -o stub/stub_crypt.o
ld -T stub/stub.ld -o stub/stub_crypt.elf stub/stub_crypt.o
objcopy -O binary stub/stub_crypt.elf stub/stub_crypt.bin

./tool/tool target/target stub/stub_crypt.bin target/packed_crypt 0xdeadbeefcafe1234
```

### As provas

De dentro: o empacotado cifrado roda igual ao original, mesma saída e mesmo código de retorno, mas agora o código trafegou cifrado no arquivo.

```
original : hello, i am the packer target -> exit 42
packed   : hello, i am the packer target -> exit 42
```

De fora: `objdump -d` na região do código do empacotado. Onde antes havia instruções legíveis, agora há os bytes cifrados. O objdump não sabe decifrar, então mostra exatamente o que alguém inspecionando o arquivo veria: nada que faça sentido.

No original, o endereço `0x402e40` (o OEP) tem o `_start` do glibc:

```
0000000000402e40 <_start>:
  402e40:  f3 0f 1e fa   endbr64
  402e44:  31 ed         xor    %ebp,%ebp
  402e46:  49 89 d1      mov    %rdx,%r9
  402e49:  5e            pop    %rsi
  402e4a:  48 89 e2      mov    %rsp,%rdx
```

No `packed_crypt`, o mesmo endereço vira lixo:

```
0000000000402e40 <_start>:
  402e40:  c7                (bad)
  402e41:  1d e0 30 de 53    sbb    $0x53de30e0,%eax
  402e46:  e4 57             in     $0x57,%al
  402e48:  e5 4c             in     $0x4c,%eax
  402e4a:  b6 43             mov    $0x43,%dh
```

Esse é o packer: o momento em que o código original some do arquivo, mas o programa ainda roda igual.

## Status

Funcional de ponta a ponta. Já existem: o alvo; a tool (validação de ELF, leitura via `mmap`, injeção do stub, escrita do bloco de metadados e cifragem opcional do segmento de código); o `shared/` com a struct de metadados e a função XOR; e três stubs (identidade, dinâmico e de cifragem). O `stub_crypt` decifra o código em runtime antes de saltar para o OEP. Evoluções naturais: cifras mais fortes que XOR, o tool patchar o OEP no stub para torná-lo genérico, e suporte a PIE (relocação em runtime).
