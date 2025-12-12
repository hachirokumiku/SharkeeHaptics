using UnityEditor;
using UnityEngine;
using VRC.SDK3.Dynamics.Contact.Components;
using System.Linq;
using System.Collections.Generic;

// Defines the data structure for each haptic position mapping
[System.Serializable]
public class HapticBone
{
    public string Name;          // e.g., "Chest"
    public string Tag;           // e.g., "Chest"
    public string ParameterName; // The VRChat OSC Parameter Name (e.g., "Receiver_Chest")
    public Transform Bone;      // The user-selected bone (Transform) for this position

    public HapticBone(string name, string tag)
    {
        Name = name;
        Tag = tag;
        // The parameter name is derived from the standard VRChat receiver name
        // This is CRUCIAL for OSC communication.
        ParameterName = $"Receiver_{name}"; 
        Bone = null;
    }
}

public class HapticReceiverPlacer : EditorWindow
{
    // *** MULTI-SLIDER VARIABLES & CONSTANTS ***
    private static float torsoRadius = 0.3f;    // Default for Torso group
    private static float armRadius = 0.3f;      // Default for Arms group
    private static float legRadius = 0.3f;      // Default for Legs group
    private static float footRadius = 0.3f;     // Default for Feet group

    private const float MIN_RADIUS = 0.02f;    // 2 cm 
    private const float MAX_RADIUS = 0.50f;    // 50 cm
    private const float HEAD_VERTICAL_OFFSET = 0.1f; // 10 cm offset for Head receiver
    // ****************************************
    
    private const string WINDOW_TITLE = "Sharkee Haptic Receiver Placer (VRC 11-Point)";
    private const string MENU_PATH = "VRC_TOOLS/Haptic Receivers/Open Haptic Placer Window";
    
    // Standard VRChat Sender Tags (Hand and Foot as requested)
    private static readonly List<string> UNIVERSAL_SENDER_TAGS = new List<string> { 
        "Hand", "Foot" 
    };

    private GameObject avatarRoot;
    private HapticBone[] hapticBones; // Array to hold the bone mappings

    // --- Standard VRChat Haptics 11-Point Position Definitions ---
    // These names are the VRChat standard for OSC Haptics parameters.
    private static readonly (string name, string tag, HumanBodyBones unityBone)[] POSITION_DEFINITIONS = new (string, string, HumanBodyBones)[]
    {
        // Name (for parameter), Tag (for collision filter), Unity Bone
        
        // Torso (2)
        ("Head", "Head", HumanBodyBones.Head),
        ("Chest", "Chest", HumanBodyBones.Chest),
        
        // Arms (2) - Typically Upper Arm is used for coverage
        ("UpperArm_L", "UpperArm_L", HumanBodyBones.LeftUpperArm),
        ("UpperArm_R", "UpperArm_R", HumanBodyBones.RightUpperArm),

        // Hips & Legs (5 + 2 Feet = 7)
        ("Hips", "Hips", HumanBodyBones.Hips), 
        ("UpperLeg_L", "UpperLeg_L", HumanBodyBones.LeftUpperLeg),
        ("UpperLeg_R", "UpperLeg_R", HumanBodyBones.RightUpperLeg),
        ("LowerLeg_L", "LowerLeg_L", HumanBodyBones.LeftLowerLeg), 
        ("LowerLeg_R", "LowerLeg_R", HumanBodyBones.RightLowerLeg),
        ("Foot_L", "Foot_L", HumanBodyBones.LeftFoot), 
        ("Foot_R", "Foot_R", HumanBodyBones.RightFoot),
    }; // Total: 11 Standard Haptic Points

    [MenuItem(MENU_PATH)]
    public static void ShowWindow()
    {
        GetWindow<HapticReceiverPlacer>(WINDOW_TITLE);
    }

    private void OnSelectionChange()
    {
        if (Selection.activeGameObject != null)
        {
            avatarRoot = Selection.activeGameObject.transform.root.gameObject;
            if (avatarRoot != null)
            {
                AutoFindBones(false); 
            }
        }
        Repaint(); // Force repaint when selection changes
    }
    
    private void OnFocus()
    {
        // Check if an avatar is already selected on focus
        if (Selection.activeGameObject != null && avatarRoot == null)
        {
            avatarRoot = Selection.activeGameObject.transform.root.gameObject;
            if (avatarRoot != null)
            {
                AutoFindBones(false);
            }
        }
        Repaint();
    }

    private void OnGUI()
    {
        InitializeBones();

        GUILayout.Label("VRChat Haptic Receiver Placer", EditorStyles.boldLabel);

        // 1. Avatar Root Selection
        EditorGUI.BeginChangeCheck();
        avatarRoot = (GameObject)EditorGUILayout.ObjectField(
            "Avatar Root (Animator)", 
            avatarRoot, 
            typeof(GameObject), 
            true
        );
        if (EditorGUI.EndChangeCheck() && avatarRoot != null)
        {
            AutoFindBones(true);
        }

        if (avatarRoot == null)
        {
            EditorGUILayout.HelpBox("Select the root GameObject of your avatar (the one with the Animator component) to begin.", MessageType.Warning);
            return;
        }

        // --- Separator and Sizing Control ---
        GUILayout.Space(10);
        
        GUILayout.Label("Receiver Sizing (Contact Radius)", EditorStyles.boldLabel);
        
        // 2. Receiver Sizing Sliders
        torsoRadius = EditorGUILayout.Slider(
            $"Torso (Head, Chest, Hips) Radius: {torsoRadius:F3} m", 
            torsoRadius,                   
            MIN_RADIUS,                      
            MAX_RADIUS                       
        );
        armRadius = EditorGUILayout.Slider(
            $"Arm (UpperArm L/R) Radius: {armRadius:F3} m", 
            armRadius,                   
            MIN_RADIUS,                      
            MAX_RADIUS                       
        );
        legRadius = EditorGUILayout.Slider(
            $"Leg (Upper/Lower Leg L/R) Radius: {legRadius:F3} m", 
            legRadius,                   
            MIN_RADIUS,                      
            MAX_RADIUS                       
        );
        footRadius = EditorGUILayout.Slider(
            $"Foot (Foot L/R) Radius: {footRadius:F3} m", 
            footRadius,                   
            MIN_RADIUS,                      
            MAX_RADIUS                       
        );
        
        // Guidance Note
        EditorGUILayout.HelpBox(
            "Adjusts the sphere radii. The default of 0.3m is a very large radius.", 
            MessageType.Info
        );
        
        // --- Separator and Bone Mappings ---
        GUILayout.Space(10);
        GUILayout.Label("Bone Mappings (Auto-Detected)", EditorStyles.boldLabel);
        
        // 3. Bone Mappings (Display & Manual Override)
        if (hapticBones != null)
        {
            foreach (var hBone in hapticBones)
            {
                hBone.Bone = (Transform)EditorGUILayout.ObjectField(
                    hBone.Name, 
                    hBone.Bone, 
                    typeof(Transform), 
                    true
                );
            }
        }

        // 4. Action Buttons
        GUILayout.Space(10);
        if (GUILayout.Button("Auto-Find Bones (Hard Reset)"))
        {
            AutoFindBones(true);
        }
        
        GUILayout.Space(5);
        
        GUI.enabled = hapticBones != null && hapticBones.All(b => b.Bone != null);
        if (GUILayout.Button("PLACE HAPTIC RECEIVERS (Clears Duplicates)"))
        {
            PlaceReceivers();
        }
        GUI.enabled = true;
    }

    private void InitializeBones()
    {
        if (hapticBones == null || hapticBones.Length != POSITION_DEFINITIONS.Length)
        {
            hapticBones = POSITION_DEFINITIONS.Select(p => new HapticBone(p.name, p.tag)).ToArray();
        }
    }

    private void AutoFindBones(bool forceReset)
    {
        InitializeBones();

        if (avatarRoot == null) return;

        Animator animator = avatarRoot.GetComponent<Animator>();
        if (animator == null || !animator.isHuman)
        {
            Debug.LogError("The selected object does not have a Humanoid Animator component.");
            return;
        }

        foreach (var pDef in POSITION_DEFINITIONS)
        {
            HapticBone hBone = hapticBones.FirstOrDefault(b => b.Name == pDef.name);

            if (hBone != null && (hBone.Bone == null || forceReset))
            {
                Transform boneTransform = animator.GetBoneTransform(pDef.unityBone);
                
                if (boneTransform != null)
                {
                    hBone.Bone = boneTransform;
                }
                else
                {
                    Debug.LogWarning($"Could not find Unity HumanBodyBone: {pDef.unityBone} for {pDef.name}");
                }
            }
        }
    }
    
    // Cleanup method to remove existing receivers, including Rcv_ and Receiver_ names
    private void CleanupExistingReceivers()
    {
        if (avatarRoot == null) return;

        // 1. Delete the main [Haptic Receivers] root object if it exists
        Transform existingRoot = avatarRoot.transform.Find("[Haptic Receivers]");
        if (existingRoot != null)
        {
            Undo.DestroyObjectImmediate(existingRoot.gameObject);
            Debug.Log("Cleaned up existing [Haptic Receivers] root object.");
        }
        
        foreach (var hBone in hapticBones)
        {
            if (hBone.Bone != null)
            {
                Transform bone = hBone.Bone;

                // 2. Delete any receiver GameObjects (Rcv_ or Receiver_) parented to the bone
                for (int i = bone.childCount - 1; i >= 0; i--)
                {
                    Transform child = bone.GetChild(i);
                    string childName = child.name;

                    if (childName.StartsWith("Receiver_") || childName.StartsWith("Rcv_"))
                    {
                        Undo.DestroyObjectImmediate(child.gameObject);
                        Debug.Log($"Cleaned up old receiver GameObject: {childName}");
                    }
                }
                
                // 3. Delete any stray VRCContactReceiver components attached directly to the bone
                var existingReceivers = bone.GetComponents<VRCContactReceiver>();
                foreach (var receiver in existingReceivers)
                {
                    Undo.DestroyObjectImmediate(receiver);
                    Debug.Log($"Cleaned up stray VRCContactReceiver component on bone: {hBone.Name}");
                }
            }
        }
    }

    private void PlaceReceivers()
    {
        if (avatarRoot == null || hapticBones == null)
        {
            EditorUtility.DisplayDialog("Error", "Avatar Root not set or bones not initialized.", "OK");
            return;
        }

        // STEP 1: CLEANUP EXISTING DUPLICATES
        CleanupExistingReceivers();

        // 2. Create a root object to hold all receivers for organization
        GameObject receiversRoot = new GameObject("[Haptic Receivers]");
        
        // Parent under the avatar root and register for undo in one call
        Undo.SetTransformParent(receiversRoot.transform, avatarRoot.transform, "Create Haptic Receivers Root"); 
        receiversRoot.transform.localPosition = Vector3.zero;

        foreach (var hBone in hapticBones)
        {
            if (hBone.Bone != null)
            {
                // Determine the correct radius based on the bone name
                float currentRadius;
                string name = hBone.Name;

                if (name == "Head" || name == "Chest" || name == "Hips")
                {
                    currentRadius = torsoRadius;
                }
                else if (name.StartsWith("UpperArm"))
                {
                    currentRadius = armRadius;
                }
                else if (name.Contains("Leg"))
                {
                    currentRadius = legRadius;
                }
                else if (name.StartsWith("Foot"))
                {
                    currentRadius = footRadius;
                }
                else
                {
                    currentRadius = torsoRadius; // Fallback
                }

                // 3. Create the receiver GameObject
                GameObject receiverObject = new GameObject($"Receiver_{hBone.Name}");
                
                // 4. Parent it to the target bone so it follows the bone, registering for undo
                Undo.SetTransformParent(receiverObject.transform, hBone.Bone, $"Parent {hBone.Name} Receiver");
                receiverObject.transform.localRotation = Quaternion.identity;
                
                Vector3 localPosition = Vector3.zero;
                
                // Logic: Snap the Head receiver to the top of the head
                if (hBone.Name == "Head")
                {
                    // Offset by a fixed amount along the local Y-axis (Up)
                    localPosition = new Vector3(0, HEAD_VERTICAL_OFFSET, 0);
                }
                
                receiverObject.transform.localPosition = localPosition;

                // 5. Add the VRC Contact Receiver component, registering for undo
                VRCContactReceiver receiver = Undo.AddComponent<VRCContactReceiver>(receiverObject);

                // 6. Configure the Receiver
                // FIX: Setting receiverType to Proximity as requested
                receiver.receiverType = VRC.SDK3.Dynamics.Contact.Components.VRCContactReceiver.ReceiverType.Proximity;
                receiver.parameter = hBone.ParameterName; // The OSC parameter (e.g., Receiver_Chest)
                
                // Set the specific radius for this area
                receiver.radius = currentRadius; 

                // Tag filtering: Only react to senders that match one of the universal tags
                receiver.allowSelf = true; 
                
                // Assign the List<string> directly
                receiver.collisionTags = UNIVERSAL_SENDER_TAGS; 
            }
            else
            {
                Debug.LogError($"Cannot place receiver for {hBone.Name}. Bone is null.");
            }
        }

        EditorUtility.DisplayDialog("Success", $"Successfully placed {hapticBones.Length} Haptic Receivers under '[Haptic Receivers]'.\n\nRadii used: Torso {torsoRadius:F3} m, Arm {armRadius:F3} m, Leg {legRadius:F3} m, Foot {footRadius:F3} m.", "OK");
    }
}