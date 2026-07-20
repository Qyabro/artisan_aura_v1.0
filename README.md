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