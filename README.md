# AURA_v1.0

- Conexion WiFi
- Conexión BT
- Conexión USB
- Salida Relé
- Solo termocupla tipo K

# Explicacion comando escritura Modbus
$$\text{write}(\underbrace{1}_{\text{Dispositivo 1}}, \underbrace{104}_{\text{REG\_RELE}}, \underbrace{1}_{\text{Valor a escribir}})$$



# Copiar proyecto Platformio
1. Duplicar la carpeta del proyecto:Ve a tu explorador de archivos (Windows, Mac o Linux) y simplemente copia y pega la carpeta completa del proyecto original. Renombra la nueva carpeta con el nombre de tu nuevo proyecto.

2. Eliminar la carpeta .pio:Paso crítico para evitar conflictos.Entra a la nueva carpeta y elimina la carpeta oculta llamada .pio. Esta carpeta contiene todos los binarios, librerías compiladas y cachés del ESP32 del proyecto anterior. Al borrarla, obligas a PlatformIO a construir el nuevo proyecto desde cero.(Opcional: también puedes borrar la carpeta oculta .vscode si quieres reiniciar las configuraciones visuales del editor para este proyecto específico).

3. Actualizar el archivo platformio.ini:Abre el archivo platformio.ini de la nueva carpeta con cualquier editor de texto simple. Si tenías un nombre personalizado para el entorno (por ejemplo, [env:mi_proyecto_anterior]), actualízalo al nuevo nombre.

4. Intyenta abrir una terminal y sale el aviso de TRUST.

# Enlazar proyecto local con GitHub desde VS Code

- Crear repo en Github

- Ejecutar comandos en VsCode
git init
git add .
git commit -m "Commit inicial sin control de relé"
git branch -M main

- Enlazar segun el link del proyecto
git remote add origin https://github.com/Qyabro/artisan_aura_basic_v1.0.git

- Subir el código a GitHub (Push)
git push -u origin main


# Crear TAGs para versiones estables

Paso 1: Crea un tag "anotado" localmente
Asegúrate de estar en tu rama main y ejecuta:
git tag -a v1.0.0 -m "Versión estable v1.0.0: Interfaz TC4 y lectura de temperatura funcionales"

Paso 2: Sube el tag a GitHub
El comando git push normal no sube los tags por defecto. Debes indicarle que lo haga:
git push origin v1.0.0

(Si alguna vez creas varios tags a la vez y quieres subirlos todos, puedes usar git push origin --tags).


# Truco para manejar versiones en GitHub
Al trabajar con firmware, no debes subir los archivos binarios compilados (.bin, .elf, .hex) directamente al repositorio de código, ya que lo vuelven muy pesado. La mejor forma de manejar los archivos listos para flashear es esta:

Sube tu Tag (v1.0.0) usando el comando del paso 2.

Ve a la página de tu repositorio en GitHub y busca la sección Releases (en la columna derecha).

Haz clic en Draft a new release.

En el desplegable "Choose a tag", selecciona tu Tag v1.0.0.

Ponle un título y en la caja de descripción escribe qué cambios tiene esta versión.

En la parte de abajo (donde dice "Attach binaries by dropping them here"), arrastra y suelta el archivo .bin que generó PlatformIO.

Dale a Publish release.